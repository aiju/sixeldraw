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
#include <sys/ioctl.h>
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
Channel *snarfch;
char *snarf;

#define MOUSEREQ "\033['|"
#define SIZEREQ "\033[14t"
#define SETTITLE "\033]0;%s\033\\"
#define WRSNARF "\033]52;s0;%.*[\033\\"
#define RDSNARF "\033]52;s0;?\033\\"

typedef struct Tagbuf Tagbuf;

enum { MAXBUF = 2*MAXWMSG };

struct Tagbuf {
	int t[32];
	int ri, wi;
	QLock ql;
};

Tagbuf mousetags, keytags, snarftags;
struct termios tiold;
int oldpgrp;
QLock sizelock;
Rectangle window;
QLock outlock;
int inited, resized, ending;
enum {
	QUIRKSIXSCR = 1,
	QUIRKMBSWAP = 2,
} quirks;
int snarfm;

void
ttyinit(void)
{
	fprint(tty, "\033[?1049h"); /* use alternate screen buffer */
	fprint(tty, "\033[1;1'z"); /* enable locator */
	fprint(tty, "\033[1'{"); /* select locator events */
	fprint(tty, "\033[?80%c", (quirks & QUIRKSIXSCR) != 0 ? 'h' : 'l'); /* reset sixel scrolling mode */
	fprint(tty, "\033[?25l"); /* hide cursor */
	fprint(tty, SIZEREQ); /* report xterm window size */
}

void
cleanup(void)
{
	int i;
	
	if(ending++) return;
	fprint(2, "cleaning up\n");
	for(i = 0; !canqlock(&outlock) && i < 10; i++)
		usleep(1000*100);
	if(i == 10) fprint(2, "failed to acquire outlock\n");
	fprint(tty, "\033\\"); /* just to be sure */
	fprint(tty, "\033[2J"); /* clear all */
	fprint(tty, "\033[0'{"); /* no locator events */
	fprint(tty, "\033[0;0'z"); /* disable locator */
	fprint(tty, "\033[?25h"); /* show cursor */
	fprint(tty, "\033[?1049l"); /* back to normal screen buffer */
	if(tcsetattr(tty, TCSAFLUSH, &tiold) < 0)
		fprint(2, "tcsetattr: %r\n");
	tcsetpgrp(tty, oldpgrp);
}

void
rawmode(int fd)
{
	struct termios ti;

	notedisable("sys: ttou");
	setpgid(0, 0);
	oldpgrp = tcgetpgrp(fd);
	tcsetpgrp(fd, getpgid(0));
	if(tcgetattr(fd, &tiold) < 0) sysfatal("tcgetattr");
	ti = tiold;
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

int
havetag(Tagbuf *t)
{
	return t->ri != t->wi;
}

int
gettag(Tagbuf *t)
{
	int r;

	r = t->t[t->ri++];
	if(t->ri == nelem(t->t))
		t->ri = 0;
	return r;
}

void
puttag(Tagbuf *t, int v)
{
	qlock(&t->ql);
	t->t[t->wi++] = v;
	if(t->wi == nelem(t->t))
		t->wi = 0;
	if(t->wi == t->ri)
		sysfatal("too many queried operations");
	qunlock(&t->ql);
}

void
matchmouse(void)
{
	Wsysmsg m;
	
	qlock(&mousetags.ql);
	while(havetag(&mousetags) && nbrecv(mousech, &m.mouse) > 0){
		m.type = Rrdmouse;
		m.tag = gettag(&mousetags);
		m.resized = resized;
		resized = 0;
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
	while(havetag(&keytags) && nbrecv(kbdch, &ul) > 0){
		m.rune = ul;
		m.type = Rrdkbd;
		m.tag = gettag(&keytags);
		replymsg(&m);
	}
	qunlock(&keytags.ql);
}

void
matchsnarf(void)
{
	Wsysmsg m;
	char *p;

	qlock(&snarftags.ql);
	while(havetag(&snarftags) && nbrecv(snarfch, &p) > 0){
		m.type = Rrdsnarf;
		m.tag = gettag(&snarftags);
		m.snarf = p;
		replymsg(&m);
	}
	qunlock(&snarftags.ql);
}

void
runmsg(Wsysmsg *m)
{
	static uchar buf[65536];
	int n;

	switch(m->type){
	case Trdmouse:
		puttag(&mousetags, m->tag);
		matchmouse();
		break;
	case Trdkbd:
		puttag(&keytags, m->tag);
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
		switch(snarfm){
		case 1: sendp(cmdch, smprint(WRSNARF, strlen(m->snarf), m->snarf)); break;
		default: snarf = strdup(m->snarf);
		}
		replymsg(m);
		break;
	case Trdsnarf:
		switch(snarfm){
		case 1: sendp(cmdch, strdup(RDSNARF)); puttag(&snarftags, m->tag); matchsnarf(); break;
		default: m->snarf = snarf; replymsg(m);
		}
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
		if(w.type == Rrdsnarf)
			free(w.snarf);
	}
}

int
ansiparse(char *s, int *f, int nf, char **imm, char *fi)
{
	int i, v;
	char *imms, fic;

	while(strchr("<=>?", *s) != nil) s++;
	for(i = 0; ; i++){
		if(*s == ';'){
			if(i < nf)
				f[i] = 0;
			s++;
			continue;
		}
		if(!isdigit(*s))
			break;
		v = strtol(s, &s, 10);
		if(i < nf)
			f[i] = v;
		if(*s == ';')
			s++;
	}
	if(*s == 0) return -1;
	imms = s;
	while(*s >= 32 && *s <= 47) s++;
	fic = *s;
	if(*s < 64 || *s > 126) return -1;
	if(imm != nil) *imm = imms;
	if(fi != nil) *fi = fic;
	return i;
}

static Mouse curmouse;

void
decmouse(char *s)
{
	int b;
	int f[4];
	char *p;
	
	if(ansiparse(s, f, nelem(f), &p, nil) < 4 || strcmp(p, "&w") != 0)
		return;
	curmouse.xy.x = f[3];
	curmouse.xy.y = f[2];
	b = f[1];
	if((quirks & QUIRKMBSWAP) == 0)
		b = b & ~7 | b << 2 & 4 | b & 2 | b >> 2 & 1;
	curmouse.buttons = b;
	nbsend(mousech, &curmouse);
	matchmouse();
}

void
windowsize(char *s)
{
	int x, y, f[3];
	char *p;

	if(ansiparse(s, f, nelem(f), &p, nil) < 3 || strcmp(p, "t") != 0)
		return;
	x = f[2];
	y = (f[1] / 6 - 1) * 6;
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
safeputs(char *s)
{
	for(; *s != 0; s++)
		if(isprint(*s))
			fprint(2, "%c", *s);
		else
			fprint(2, "\\%#o", (uchar)*s);
	fprint(2, "\n");
}

void
snarfresp(char *s)
{
	char *p, *q;
	int n, rc;
	char *v;

	s = strchr(s, ';');
	if(s == nil) return;
	s = strchr(s+1, ';');
	if(s == nil) return;
	s++;
	for(p = s, q = s; *p != 0;)
		if(*p == 033){
			p++;
			if(*p == 0) break;
			if(*p == '['){
				do
					p++;
				while(*p > 0 && (*p < 64 || *p > 126));
				if(*p == 0) break;
			}
			p++;
		}else
			*q++ = *p++;
	*q = 0;

	n = (strlen(s) + 3) * 3 / 4 + 10;
	v = malloc(n);
	rc = dec64((uchar *) v, n - 1, s, strlen(s));
	if(rc < 0){
		fprint(2, "base64 decode failed: %s\n", s);
		*v = 0;
	}else{
		v[rc] = 0;
	}
	nbsendp(snarfch, v);
	matchsnarf();
}

Rune ss3keys[] = {
	['A'] Kup,
	['B'] Kdown,
	['C'] Kleft,
	['D'] Kright,
	[' '] ' ',
	['I'] '\t',
	['M'] '\r',
	['P'] KF|1,
	['Q'] KF|2,
	['R'] KF|3,
	['S'] KF|4,
};

Rune csikeys[] = {
	[1] Khome,
	[2] Kins,
	[3] Kdel,
	[4] Kend,
	[5] Kpgup,
	[6] Kpgdown,
	[11] KF|1,
	[12] KF|2,
	[13] KF|3,
	[14] KF|4,
	[15] KF|5,
	[17] KF|6,
	[18] KF|7,
	[19] KF|8,
	[20] KF|9,
	[21] KF|10,
	[23] KF|11,
	[24] KF|12,
};

static void
kbdkey(int c)
{
	static Rune buf[32];
	static Rune *p;
	int r;
	Rune *q;
	
	if(c == Kalt)
		p = buf;
	else if(p != nil){
		*p++ = c;
		*p = 0;
		r = _latin1(buf, p - buf);
		if(p == buf + sizeof(buf) - 1 || r == -1){
			for(q = buf; q < p; q++)
				nbsendul(kbdch, *q);
			p = nil;
		}else if(r > 0){
			nbsendul(kbdch, r);
			p = nil;
		}
	}else
		nbsendul(kbdch, c);
}

void
ttyinproc(void *arg)
{
	int c, r;
	int f[10];
	static char buf[256];
	char *p;
	Rune ru;

	for(;;){
		c = Bgetc(ttyin);
		if(c < 0) threadexitsall(nil);
		switch(c){
		case 033:
			c = Bgetc(ttyin);
			if(c == 'O'){
				c = Bgetc(ttyin);
				if(c < 0) break;
				if(c >= nelem(ss3keys) || (r = ss3keys[c]) == 0)
					fprint(2, "unknown key SS3 %c\n", c);
				else
					kbdkey(r);
				break;
			}
			if(c == ']'){
				p = Brdstr(ttyin, '\\', 1);
				if(strncmp(p, "52;", 3) == 0)
					snarfresp(p);
				free(p);
				break;
			}
			if(c != '['){
				kbdkey(Kalt);
				kbdkey(c);
				break;
			}
			p = buf;
			while(c = Bgetc(ttyin), c >= 0){
				if(p < buf + sizeof(buf) - 1)
					*p++ = c;
				if(c >= 64 && c <= 126) break;
			}
			*p = 0;
			switch(c){
			case 'A': kbdkey(Kup); break;
			case 'B': kbdkey(Kdown); break;
			case 'C': kbdkey(Kright); break;
			case 'D': kbdkey(Kleft); break;
			case 'H': kbdkey(Khome); break;
			case 'F': kbdkey(Kend); break;
			case 'w': decmouse(buf); break;
			case 't': windowsize(buf); break;
			case '~':
				if(ansiparse(buf, f, nelem(f), nil, nil) < 1)
					break;
				if(f[0] >= nelem(csikeys) || (r = csikeys[f[0]]) == 0)
					fprint(2, "unknown key ESC %d ~\n", f[0]);
				else
					kbdkey(r);
				break;
			default: fprint(2, "unknown control %c\n", c);
			}
			break;
		case 13:
			kbdkey(10);
			break;
		default:
			if(c < 0)
				threadexitsall(nil);
			if(c >= 0x80){
				p = buf;
				*p++ = c;
				while(!fullrune(buf, p - buf)){
					c = Bgetc(ttyin);
					if(c < 0) break;
					*p++ = c;
				}
				chartorune(&ru, buf);
				kbdkey(ru);
			}else
				kbdkey(c);
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
	fmtinstall('W', drawfcallfmt);
	fmtinstall('[', encodefmt);

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
	
	if(getenv("QUIRKS") != nil)
		quirks = atoi(getenv("QUIRKS"));
	if(getenv("SNARF") != nil)
		snarfm = atoi(getenv("SNARF"));

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
	snarfch = chancreate(sizeof(char *), 32);

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
