#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>
#include <stdio.h>
#include <errno.h>
#include <draw.h>
#include <memdraw.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>
#include <drawfcall.h>
#include <ctype.h>
#include <termios.h>
#include "devdraw.h"

int tty;
Biobuf *fdin;
Biobuf *ttyin, *ttyout;
Memimage *_img;
Channel *msgout;
Channel *flushch;
Channel *cmdch;
Channel *mousech;
Channel *kbdch;
char *snarf;

#define MOUSEREQ "\033['|"
#define SIZEREQ "\033[14t"
#define SETTITLE "\033]0;%s\033\\"

typedef struct Tagbuf Tagbuf;

enum { MAXBUF = 2*MAXWMSG };

struct Tagbuf {
	int t[32];
	int ri, wi;
	QLock ql;
};

Tagbuf mousetags, keytags;
struct termios tiold;
QLock sizelock;
Rectangle window;
QLock outlock;
int inited, resized, ending;

void
rawmode(int fd)
{
	struct termios ti;
	
	if(tcgetattr(fd, &ti) < 0) sysfatal("tcgetattr");
	tiold = ti;
	cfmakeraw(&ti);
	if(tcsetattr(fd, TCSAFLUSH, &ti) < 0) sysfatal("tcsetattr");
}

void
outproc(void *arg)
{
	Wsysmsg w;
	static uchar buf[MAXWMSG];
	int n;

	for(;;){
		if(recv(msgout, &w) < 0)
			sysfatal("recv: %r");
		n = convW2M(&w, buf, sizeof(buf));
		if(n == 0) continue;
		if(write(4, buf, n) < n)
			sysfatal("write: %r");
		
	}
}

void
replymsg(Wsysmsg *m)
{
	if((m->type & 1) == 0)
		m->type++;
	send(msgout, m);
}

void
replyerror(Wsysmsg *m)
{
	char err[ERRMAX];
	
	rerrstr(err, sizeof(err));
	m->type = Rerror;
	m->error = err;
	replymsg(m);
}

void
matchmouse(void)
{
	Wsysmsg m;
	
	qlock(&mousetags.ql);
	while(mousetags.ri != mousetags.wi && nbrecv(mousech, &m.mouse) > 0){
		m.type = Rrdmouse;
		m.tag = mousetags.t[mousetags.ri++];
		if(mousetags.ri == nelem(mousetags.t))
			mousetags.ri = 0;
		m.resized = resized;
		resized = 0;
		print("sending %d\n", resized);
		replymsg(&m);
	}
	qunlock(&mousetags.ql);
}

void
matchkbd(void)
{
	Wsysmsg m;
	ulong ul;
	
	qlock(&keytags.ql);
	while(keytags.ri != keytags.wi && nbrecv(kbdch, &ul) > 0){
		m.rune = ul;
		m.type = Rrdkbd;
		m.tag = keytags.t[keytags.ri++];
		if(keytags.ri == nelem(keytags.t))
			keytags.ri = 0;
		replymsg(&m);
	}
	qunlock(&keytags.ql);
}

void
runmsg(Wsysmsg *m)
{
	static uchar buf[65536];
	int n;

	switch(m->type){
	case Trdmouse:
		qlock(&mousetags.ql);
		mousetags.t[mousetags.wi++] = m->tag;
		if(mousetags.wi == nelem(mousetags.t))
			mousetags.wi = 0;
		if(mousetags.wi == mousetags.ri)
			sysfatal("too many queued mouse reads");
		qunlock(&mousetags.ql);
		matchmouse();
		break;
	case Trdkbd:
		qlock(&keytags.ql);
		keytags.t[keytags.wi++] = m->tag;
		if(keytags.wi == nelem(keytags.t))
			keytags.wi = 0;
		if(keytags.wi == keytags.ri)
			sysfatal("too many queued keyboard reads");
		qunlock(&keytags.ql);
		matchkbd();
		break;
	case Tinit:
		memimageinit();
		qlock(&sizelock);
		_img = _allocmemimage(window, CMAP8);
		qunlock(&sizelock);
		_initdisplaymemimage(_img);
		replymsg(m);
		inited++;
		if(m->label != nil)
			sendp(cmdch, smprint(SETTITLE, m->label));
		break;
	case Trddraw:
		n = m->count > sizeof(buf) ? sizeof(buf) : m->count;
		if(n = _drawmsgread(buf, n), n < 0)
			replyerror(m);
		else{
			m->data = buf;
			m->count = n;
			replymsg(m);
		}
		break;
	case Twrdraw:
		if(_drawmsgwrite(m->data, m->count) < 0)
			replyerror(m);
		else
			replymsg(m);
		break;
	case Tlabel:
		sendp(cmdch, smprint(SETTITLE, m->label));
		break;
	case Twrsnarf:
		free(snarf);
		snarf = strdup(m->snarf);
		replymsg(m);
		break;
	case Trdsnarf:
		m->snarf = snarf;
		replymsg(m);
		break;
	default:
		fprint(2, "unknown message %W\n", m);
	case Tmoveto:
	case Tbouncemouse:
		m->type = Rerror;
		m->error = "not implemented";
		replymsg(m);
	}
}

void
msgproc(void *arg)
{
	int rc;
	Wsysmsg w;
	static uchar buf[MAXWMSG];
	uchar *p;
	
	p = buf;
	for(;;){
		rc = Bgetc(fdin);
		if(rc < 0){
			cleanup();
			threadexitsall(nil);
		}
		*p++ = rc;
		rc = convM2W(buf, p - buf, &w);
		if(rc > 0){
			runmsg(&w);
			p -= rc;
		}
	}
}

static Mouse curmouse;

void
decmouse(char *s)
{
	int b, x, y;
	char *p;
	
	p = s;
	strtol(s, &p, 10);
	if(*p != ';') return;
	b = strtol(p + 1, &p, 10);
	if(*p != ';') return;
	y = strtol(p + 1, &p, 10);
	if(*p != ';') return;
	x = strtol(p + 1, &p, 10);
	if(*p == ';')
		strtol(p + 1, &p, 10);
	if(strcmp(p, "&w") != 0) return;
	curmouse.xy.x = x;
	curmouse.xy.y = y;
	curmouse.buttons = b << 2 & 4 | b & 2 | b >> 2 & 1;
	nbsend(mousech, &curmouse);
	matchmouse();
}

void
windowsize(char *s)
{
	int x, y;
	char *p;

	p = s;
	strtol(s, &p, 10);
	if(*p != ';') return;
	y = strtol(p + 1, &p, 10);
	if(*p != ';') return;
	x = strtol(p + 1, &p, 10);
	if(strcmp(p, "t") != 0) return;
	y = (y / 6 - 1) * 6;
	if(inited && window.max.x == x && window.max.y == y) return;
	window.max.x = x;
	window.max.y = y;
	if(!inited)
		qunlock(&sizelock);
	else{
		_img = allocmemimage(window, CMAP8);
		_drawreplacescreenimage(_img);
		resized++;
		nbsend(mousech, &curmouse);
		matchmouse();
	}
}

void
ttyinproc(void *arg)
{
	int c;
	static char buf[256];
	char *p;

	for(;;){
		c = Bgetc(ttyin);
		switch(c){
		case 3:
			threadexitsall(nil);
			break;
		case 033:
			c = Bgetc(ttyin);
			if(c != '[') break;
			p = buf;
			while(c = Bgetc(ttyin), c >= 0){
				if(p < buf + sizeof(buf) - 1)
					*p++ = c;
				if(isalpha(c)) break;
			}
			*p = 0;
			switch(c){
			case 'A': nbsendul(kbdch, Kup); break;
			case 'B': nbsendul(kbdch, Kdown); break;
			case 'C': nbsendul(kbdch, Kright); break;
			case 'D': nbsendul(kbdch, Kleft); break;
			case 'H': nbsendul(kbdch, Khome); break;
			case 'F': nbsendul(kbdch, Kend); break;
			case 'w': decmouse(buf); break;
			case 't': windowsize(buf); break;
			default: fprint(2, "unknown control %c\n", c);
			}
			break;
		case 13:
			nbsendul(kbdch, 10);
			break;
		default:
			if(c < 0)
				threadexitsall(nil);
			nbsendul(kbdch, c);
		}
		matchkbd();
	}
}

void
flush(Rectangle r)
{
	int i, x, y, c, b, v, m;
	int lastch, lastn;
	static int maxcol[256];
	static uchar *col[6];

	qlock(&outlock);
restart:
	Bprint(ttyout, "\033Pq");
	for(i = 0; i < 256; i++){
		v = cmap2rgb(i);
		Bprint(ttyout, "#%d;2;%d;%d;%d", i, (v >> 16 & 0xff) * 100 / 255, (v >> 8 & 0xff) * 100 / 255, (v & 0xff) * 100 / 255);
	}
	if(r.max.y > _img->r.max.y) r.max.y = _img->r.max.y;
	r = _img->r;
	col[0] = malloc(_img->r.max.x * 6);
	for(i = 1; i < 6; i++)
		col[i] = col[i-1] + _img->r.max.x;
	for(y = 0; y < r.max.y; y += 6){
		if(nbrecv(flushch, &r) > 0){
			Bprint(ttyout, "\033\\");
			goto restart;
		}
		if(y + 5 < r.min.y)
			goto next;
		_unloadmemimage(_img, Rect(0, y, _img->r.max.x, y + 6), col[0], _img->r.max.x * 6);
		memset(maxcol, 0, sizeof maxcol);
		for(x = 0; x < Dx(_img->r); x++)
			for(i = 0; i < 6; i++)
				maxcol[col[i][x]] = x + 1;
		for(c = 0; c < 256; c++){
			if(maxcol[c] == 0) continue;
			Bprint(ttyout, "#%d", c);
			lastch = -1;
			lastn = 0;
			for(x = 0; x < maxcol[c]; x++){
				b = 0;
				m = ~63;
				for(i = 0; i < 6; i++){
					if(col[i][x] == c)
						b |= 1<<i;
					if(col[i][x] <= c)
						m |= 1<<i;
				}
				if(((lastch ^ b) & m) == 0)
					lastn++;
				else{
					if(lastn >= 4)
						Bprint(ttyout, "!%d%c", lastn, 63 + lastch);
					else
						while(lastn--)
							Bputc(ttyout, 63 + lastch);
					lastn = 1;
					lastch = b;
				}
			}
			if(lastn >= 4)
				Bprint(ttyout, "!%d%c", lastn, 63 + lastch);
			else
				while(lastn--)
					Bputc(ttyout, 63 + lastch);
			Bputc(ttyout, '$');
		}
	next:
		Bputc(ttyout, '-');
	}
	free(col[0]);
	Bprint(ttyout, "\033\\");
	Bflush(ttyout);
	qunlock(&outlock);
}

void
ttyoutproc(void *arg)
{
	Rectangle r;
	int rc;
	char *str;

	for(;;){
		Alt a[] = {
			{flushch, &r, CHANRCV},
			{cmdch, &str, CHANRCV},
			{nil, nil, CHANEND}
		};
		rc = alt(a);
		if(ending) return;
		switch(rc){
		case 0: flush(r); break;
		case 1:
			qlock(&outlock);
			Bprint(ttyout, "%s", str);
			Bflush(ttyout);
			qunlock(&outlock);
			free(str);
			break;
		}
	}
}

void
ttyinit(void)
{
	fprint(tty, "\033[?1049h"); /* use alternate screen buffer */
	fprint(tty, "\033[1;1'z"); /* enable locator */
	fprint(tty, "\033[1'{"); /* select locator events */
	fprint(tty, "\033[?80l"); /* reset sixel scrolling mode */
	fprint(tty, "\033[?25l"); /* hide cursor */
	fprint(tty, SIZEREQ); /* report xterm window size */
}

void
cleanup(void)
{
	int i;
	
	if(ending++) return;
	for(i = 0; !canqlock(&outlock) && i <= 10; i++)
		usleep(1000*100);
	fprint(tty, "\033\\"); /* just to be sure */
	fprint(tty, "\033[2J"); /* clear all */
	fprint(tty, "\033[0'{"); /* no locator events */
	fprint(tty, "\033[0;0'z"); /* disable locator */
	fprint(tty, "\033[?25h"); /* show cursor */
	fprint(tty, "\033[?1049l"); /* back to normal screen buffer */
	tcsetattr(tty, TCSAFLUSH, &tiold);
}

void
mousereqproc(void *arg)
{
	int n;

	n = 0;
	for(;;){
		usleep(20*1000);
		sendp(cmdch, strdup(MOUSEREQ));
		if(++n == 10){
			n = 0;
			sendp(cmdch, strdup(SIZEREQ));
		}
	}
}

int
notehand(void *ureg, char *note)
{
	cleanup();
	return 0;
}

void
threadmain(int argc, char **argv)
{
	int debugfd;

	fmtinstall('W', drawfcallfmt);

	ARGBEGIN {
	default: break;
	} ARGEND;

	dup(0, 3);
	dup(1, 4);
	close(0);
	close(1);
	close(2);
	open("/dev/null", OREAD);
	open("/dev/null", OWRITE);
	if(getenv("SIXELDBG") != nil)
		open(getenv("SIXELDBG"), OWRITE);
	else
		open("/dev/null", OWRITE);

	tty = open("/dev/tty", ORDWR);
	if(tty < 0) sysfatal("open: %r");
	rawmode(tty);
	
	atexit(cleanup);
	ttyinit();
	qlock(&sizelock);
	
	fdin = Bfdopen(3, OREAD);
	if(fdin == nil) sysfatal("Bfdopen: %r");
	ttyin = Bfdopen(tty, OREAD);
	if(ttyin == nil) sysfatal("Bfdopen: %r");
	ttyout = Bfdopen(tty, OWRITE);
	if(ttyout == nil) sysfatal("Bfdopen: %r");
	msgout = chancreate(sizeof(Wsysmsg), 2);
	flushch = chancreate(sizeof(Rectangle), 0);
	cmdch = chancreate(sizeof(char *), 16);
	mousech = chancreate(sizeof(Mouse), 32);
	kbdch = chancreate(sizeof(ulong), 256);

	threadnotify(notehand, 1);
	
	proccreate(outproc, nil, mainstacksize);
	proccreate(ttyinproc, nil, mainstacksize);
	proccreate(ttyoutproc, nil, mainstacksize);
	proccreate(mousereqproc, nil, mainstacksize);
	
	msgproc(nil);
}

void
_flushmemscreen(Rectangle r)
{
	send(flushch, &r);
}

int
cloadmemimage(Memimage *i, Rectangle r, uchar *data, int ndata)
{
	return _cloadmemimage(i, r, data, ndata);
}

int
loadmemimage(Memimage *i, Rectangle r, uchar *data, int ndata)
{
	return _loadmemimage(i, r, data, ndata);
}

int
unloadmemimage(Memimage *i, Rectangle r, uchar *data, int ndata)
{
	return _unloadmemimage(i, r, data, ndata);
}
