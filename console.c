// Console input and output.
// Input is from the keyboard or serial port.
// Output is written to the screen and serial port.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "traps.h"
#include "spinlock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

#define UP_ARROW 226
#define DOWN_ARROW 227
#define LEFT_ARROW 228
#define RIGHT_ARROW 229

#define MAX_HISTORY 16

static void consputc(int);

static int panicked = 0;

static struct {
  struct spinlock lock;
  int locking;
} cons;

static void
printint(int xx, int base, int sign)
{
  static char digits[] = "0123456789abcdef";
  char buf[16];
  int i;
  uint x;

  if(sign && (sign = xx < 0))
    x = -xx;
  else
    x = xx;

  i = 0;
  do{
    buf[i++] = digits[x % base];
  }while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  while(--i >= 0)
    consputc(buf[i]);
}
//PAGEBREAK: 50

// Print to the console. only understands %d, %x, %p, %s.
void
cprintf(char *fmt, ...)
{
  int i, c, locking;
  uint *argp;
  char *s;

  locking = cons.locking;
  if(locking)
    acquire(&cons.lock);

  if (fmt == 0)
    panic("null fmt");

  argp = (uint*)(void*)(&fmt + 1);
  for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
    if(c != '%'){
      consputc(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if(c == 0)
      break;
    switch(c){
    case 'd':
      printint(*argp++, 10, 1);
      break;
    case 'x':
    case 'p':
      printint(*argp++, 16, 0);
      break;
    case 's':
      if((s = (char*)*argp++) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
      break;
    case '%':
      consputc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c);
      break;
    }
  }

  if(locking)
    release(&cons.lock);
}

void
panic(char *s)
{
  int i;
  uint pcs[10];

  cli();
  cons.locking = 0;
  cprintf("cpu%d: panic: ", cpu->id);
  cprintf(s);
  cprintf("\n");
  getcallerpcs(&s, pcs);
  for(i=0; i<10; i++)
    cprintf(" %p", pcs[i]);
  panicked = 1; // freeze other CPU
  for(;;)
    ;
}

//PAGEBREAK: 50
#define BACKSPACE 0x100
#define CRTPORT 0x3d4
static ushort *crt = (ushort*)P2V(0xb8000);  // CGA memory

static void
cgaputc(int c)
{
  int pos;

  // Cursor position: col + 80*row.
  outb(CRTPORT, 14);
  pos = inb(CRTPORT+1) << 8;
  outb(CRTPORT, 15);
  pos |= inb(CRTPORT+1);

  switch(c) {
    case '\n':
      pos += 80 - pos%80;
      break;
    case BACKSPACE:
      if(pos > 0) --pos;
      break;
    case LEFT_ARROW:
      if(pos > 0) --pos;
      break;
    default:
      crt[pos++] = (c&0xff) | 0x0700;  // black on white
  }

  if(pos < 0 || pos > 25*80)
    panic("pos under/overflow");

  if((pos/80) >= 24){  // Scroll up.
    memmove(crt, crt+80, sizeof(crt[0])*23*80);
    pos -= 80;
    memset(crt+pos, 0, sizeof(crt[0])*(24*80 - pos));
  }

  outb(CRTPORT, 14);
  outb(CRTPORT+1, pos>>8);
  outb(CRTPORT, 15);
  outb(CRTPORT+1, pos);
  if (c == BACKSPACE)
    crt[pos] = ' ' | 0x0700;
}

void
consputc(int c)
{
  if(panicked){
    cli();
    for(;;)
      ;
  }

  switch (c) {
    case BACKSPACE:
      // uartputc prints to Linux's terminal
      uartputc('\b'); uartputc(' '); uartputc('\b');
      break;
    case LEFT_ARROW:
      uartputc('\b');
      break;
    default:
      uartputc(c);
  }
  // cgaputc prints to QEMU's terminal
  cgaputc(c);
}

#define INPUT_BUF 128
struct {
  char buf[INPUT_BUF];
  uint r;  // Read index, exec will start reading the command from here
  uint w;  // Write index, exec will finish reading the command here
  uint e;  // Edit index, current caret position
  uint rightmost; // position in buf for the next char
} input;

char buf2[INPUT_BUF];  // temporary storage for input.buf in a certain context

struct {
  char entries[MAX_HISTORY][INPUT_BUF + 1];
  uint population;
  uint current;
} records;

#define C(x)  ((x)-'@')  // Control-x

/*
Copy input.buf to a safe location. Used only when punching in new keys and the
caret isn't at the end of the line.
*/
void copybuf() {
  uint n = input.rightmost - input.e;
  uint i;
  for (i = 0; i < n; i++)
    buf2[i] = input.buf[input.e + i % INPUT_BUF];
}

/*
Shift input.buf one byte to the right, and repaint the chars on-screen. Used
Used only when punching in new keys and the caret isn't at the end of the line.
*/
void shiftbufright() {
  uint n = input.rightmost - input.e;
  int i;
  for (i = 0; i < n; i++) {
    char c = buf2[i];
    input.buf[input.e + i % INPUT_BUF] = c;
    consputc(c);
  }
  // reset buf2 for future use
  memset(buf2, '\0', INPUT_BUF);
  // return the caret to its correct position
  for (i = 0; i < n; i++) {
    consputc(LEFT_ARROW);
  }
}

/*
Shift input.buf one byte to the left, and repaint the chars on-screen. Used
Used only when punching in BACKSPACE and the caret isn't at the end of the line.
*/
void shiftbufleft() {
  uint n = input.rightmost - input.e;
  uint i;
  consputc(LEFT_ARROW);
  input.e--;
  for (i = 0; i < n; i++) {
    char c = input.buf[input.e + i + 1 % INPUT_BUF];
    input.buf[input.e + i % INPUT_BUF] = c;
    consputc(c);
  }
  input.rightmost--;
  consputc(' '); // delete the last char in line
  for (i = 0; i <= n; i++) {
    consputc(LEFT_ARROW); // shift the caret back to the left
  }
}

void
consoleintr(int (*getc)(void))
{
  int c, doprocdump = 0;
  uint i, n, entriesize;
  acquire(&cons.lock);
  while((c = getc()) >= 0){
    switch(c){
    case C('P'):  // Process listing.
      doprocdump = 1;   // procdump() locks cons.lock indirectly; invoke later
      break;
    case C('U'):  // Kill line.
      if (input.rightmost > input.e) { // caret isn't at the end of the line
        uint numtoshift = input.rightmost - input.e;
        uint placestoshift = input.e - input.w;
        for (i = 0; i < placestoshift; i++) {
          consputc(LEFT_ARROW);
        }
        memmove(input.buf + input.w, input.buf + input.w + placestoshift, numtoshift);
        input.e -= placestoshift;
        input.rightmost -= placestoshift;
        for (i = 0; i < numtoshift; i++) { // repaint the chars
          consputc(input.buf[input.e + i % INPUT_BUF]);
        }
        for (i = 0; i < placestoshift; i++) { // erase the leftover chars
          consputc(' ');
        }
        for (i = 0; i < placestoshift + numtoshift; i++) { // move the caret back to the left
          consputc(LEFT_ARROW);
        }
      }
      else { // caret is at the end of the line
        while(input.e != input.w &&
              input.buf[(input.e-1) % INPUT_BUF] != '\n'){
          input.e--;
          input.rightmost--;
          consputc(BACKSPACE);
        }
      }
      break;
    case C('H'): case '\x7f':  // Backspace
      if (input.rightmost != input.e && input.e != input.w) { // caret isn't at the end of the line
        shiftbufleft();
        break;
      }
      if(input.e != input.w){ // caret is at the end of the line
        input.e--;
        input.rightmost--;
        consputc(BACKSPACE);
      }
      break;
    case LEFT_ARROW:
      if (input.e != input.w) {
        input.e--;
        consputc(c);
      }
      break;
    case RIGHT_ARROW:
      if (input.e < input.rightmost) {
        consputc(input.buf[input.e]);
        input.e++;
      }
      else if (input.e == input.rightmost){
        consputc(' ');
        consputc(LEFT_ARROW);
      }
      break;
    case UP_ARROW:
      if (records.current > 0) {
        records.current--;
        n = input.rightmost - input.w;  // current line on screen length
        entriesize = records.entries[records.current][INPUT_BUF];
        for (i = 0; i < input.e - input.w; i++) {
          consputc(LEFT_ARROW); // move the caret to the beginning of the line
        }
        for (i = 0; i < entriesize; i++) {
          input.buf[input.w + i % INPUT_BUF] = records.entries[records.current][i]; // repopulate the buffer
        }
        input.e = input.w + records.entries[records.current][INPUT_BUF]; // index INPUT_BUF is the length of the command
        input.rightmost = input.e;
        for (i = 0; i < entriesize; i++) {
          consputc(input.buf[input.w + i % INPUT_BUF]); /// repaint the new command
        }
        n = records.entries[records.current][INPUT_BUF] - n;
        for (i = 0; i < entriesize - n; i++) {
          consputc(' '); // erase chars from the old command
        }
        for (i = 0; i < entriesize - n; i++) {
          consputc(LEFT_ARROW); // move the caret back to the left
        }
      }
      break;
    case DOWN_ARROW:
      if (records.current <= records.population - 1) {
        records.current++;
        n = input.rightmost - input.w;  // current line on screen length
        entriesize = records.entries[records.current][INPUT_BUF];
        for (i = 0; i < input.e - input.w; i++) {
          consputc(LEFT_ARROW); // move the caret to the beginning of the line
        }
        for (i = 0; i < entriesize; i++) {
          input.buf[input.w + i % INPUT_BUF] = records.entries[records.current][i]; // repopulate the buffer
        }
        input.e = input.w + records.entries[records.current][INPUT_BUF]; // index INPUT_BUF is the length of the command
        input.rightmost = input.e;
        for (i = 0; i < entriesize; i++) {
          consputc(input.buf[input.w + i % INPUT_BUF]); /// repaint the new command
        }
        n = records.entries[records.current][INPUT_BUF] - n;
        for (i = 0; i < entriesize - n; i++) {
          consputc(' '); // erase chars from the old command
        }
        for (i = 0; i < entriesize - n; i++) {
          consputc(LEFT_ARROW); // move the caret back to the left
        }
      }
      else { // clear the line
        n = input.rightmost - input.w;
        for (i = 0; i < n; i++) {
          consputc(' ');
        }
        for (i = 0; i < n; i++) {
          consputc(LEFT_ARROW);
        }
      }
      break;
    case '\n':
    case '\r':
        input.e = input.rightmost;
    default:
      if(c != 0 && input.e-input.r < INPUT_BUF){
        c = (c == '\r') ? '\n' : c;
        if (input.rightmost > input.e) { // caret isn't at the end of the line
          copybuf();
          input.buf[input.e++ % INPUT_BUF] = c;
          input.rightmost++;
          consputc(c);
          shiftbufright();
        }
        else {
          input.buf[input.e++ % INPUT_BUF] = c;
          input.rightmost = input.e - input.rightmost == 1 ? input.e : input.rightmost;
          consputc(c);
        }
        if(c == '\n' || c == C('D') || input.e == input.r+INPUT_BUF){
          input.w = input.rightmost;
          if (records.population < MAX_HISTORY) { // empty places in records.entries
            n = input.rightmost - input.r;
            n = input.buf[input.rightmost - 1] == '\n' ? n - 1 : n;
            records.current = records.population;
            memmove(records.entries[records.current], input.buf + input.r, n);
            records.entries[records.current][INPUT_BUF] = n;
            records.population++;
            records.current++;
          }
          else { // shift all entries one index down to make room for a new one
            memmove(records.entries[0], records.entries[1], (MAX_HISTORY - 1) * (INPUT_BUF + 1));
            records.current--;
            n = input.rightmost - input.r;
            n = input.buf[input.rightmost - 1] == '\n' ? n - 1 : n;
            memmove(records.entries[MAX_HISTORY - 1], input.buf + input.r, n);
            records.entries[MAX_HISTORY - 1][INPUT_BUF] = n;
            records.current = records.population;
          }
          wakeup(&input.r);
        }
      }
      break;
    }
  }
  release(&cons.lock);
  if(doprocdump) {
    procdump();  // now call procdump() wo. cons.lock held
  }
}

int history(char *buffer, int historyId) {
  if (historyId < 0 || historyId > MAX_HISTORY - 1)
    return -2;
  if (historyId >= records.current)
    return -1;
  memset(buffer, '\0', INPUT_BUF);
  memmove(buffer, records.entries[historyId], records.entries[historyId][INPUT_BUF]);
  return 0;
}

int
consoleread(struct inode *ip, char *dst, int n)
{
  uint target;
  int c;

  iunlock(ip);
  target = n;
  acquire(&cons.lock);
  while(n > 0){
    while(input.r == input.w){
      if(proc->killed){
        release(&cons.lock);
        ilock(ip);
        return -1;
      }
      sleep(&input.r, &cons.lock);
    }
    c = input.buf[input.r++ % INPUT_BUF];
    if(c == C('D')){  // EOF
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        input.r--;
      }
      break;
    }
    *dst++ = c;
    --n;
    if(c == '\n')
      break;
  }
  release(&cons.lock);
  ilock(ip);

  return target - n;
}

int
consolewrite(struct inode *ip, char *buf, int n)
{
  int i;

  iunlock(ip);
  acquire(&cons.lock);
  for(i = 0; i < n; i++)
    consputc(buf[i] & 0xff);
  release(&cons.lock);
  ilock(ip);

  return n;
}

void
consoleinit(void)
{
  initlock(&cons.lock, "console");

  devsw[CONSOLE].write = consolewrite;
  devsw[CONSOLE].read = consoleread;
  cons.locking = 1;

  picenable(IRQ_KBD);
  ioapicenable(IRQ_KBD, 0);
}
