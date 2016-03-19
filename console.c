// Console input and output.
// Input is from the keyboard or serial port.
// Output is written to the screen and serial port.
#include "console.h"
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



static void consputc(int);

static int panicked = 0;

static struct {
  struct spinlock lock;
  int locking;
} cons;

/*
  prints an int to screen, used for cprintf
*/
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


/*
  this methods prints the meassage parameter and freezes the cpu
*/
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

static ushort *crt = (ushort*)P2V(0xb8000);  // CGA memory


/*
  this method actually prints the character to the xv6 screen
*/
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
   /* case RIGHT_ARROW:                                  //GILAD maybe use something like this to move right... could couse problems with uart
      pos++;
      crt[pos] = (crt[pos]&0xff) | 0x0700;  // black on white
      break;*/
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

/*
    makes sure both the linux shel screen and the qemu will handle the given char proparly
*/
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
      uartputc('\b'); uartputc(' '); uartputc('\b');  // uart is writing to the linux shell
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


struct {
  char buf[INPUT_BUF];
  uint r;  // Read index, exec will start reading the command from here
  uint w;  // Write index, exec will finish reading the command here
  uint e;  // Edit index, current caret position
  uint rightmost; // the first empty char in the line
} input;

char charsToBeMoved[INPUT_BUF];  // temporary storage for input.buf in a certain context

#define C(x)  ((x)-'@')  // Control-x

/*
Copy input.buf to a safe location. Used only when punching in new keys and the
caret isn't at the end of the line.
*/
void copyCharsToBeMoved() {
  uint n = input.rightmost - input.e;
  uint i;
  for (i = 0; i < n; i++)
    charsToBeMoved[i] = input.buf[input.e + i % INPUT_BUF];
}

/*
Shift input.buf one byte to the right, and repaint the chars on-screen. Used
Used only when punching in new keys and the caret isn't at the end of the line.
*/
void shiftbufright() {
  uint n = input.rightmost - input.e;
  int i;
  for (i = 0; i < n; i++) {
    char c = charsToBeMoved[i];
    input.buf[input.e + i % INPUT_BUF] = c;
    consputc(c);
  }
  // reset charsToBeMoved for future use
  memset(charsToBeMoved, '\0', INPUT_BUF);
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


/*
  this method begins the handeling of a console interupt
*/
void
consoleintr(int (*getc)(void))
{
  int c, doprocdump = 0;
  uint i, n;
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
          uint i;
          for (i = 0; i < placestoshift; i++) {
            consputc(LEFT_ARROW);
          }
          memmove(input.buf + input.w, input.buf + input.w + placestoshift, numtoshift); //copying the remaining chars to the beginning
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
        else { // caret is at the end of the line -                                       ( deleting everything from both screen and inputbuf)
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
        if(input.e != input.w){ // caret is at the end of the line - deleting last char
          input.e--;
          input.rightmost--;
          consputc(BACKSPACE);
        }
        break;
      case LEFT_ARROW:
        if (input.e != input.w) {
          input.e--;
          consputc(c);   // consputx gets a left-arrow, and just move the blinker left
        }
        break;
      case RIGHT_ARROW: //a bit long way to move the blinker right (moving n-times right and)
       if (input.e < input.rightmost) {
          n = input.rightmost - input.e;
          for (i = 0; i < n; i++) {
            consputc(input.buf[input.e + i % INPUT_BUF]);
          }
          for (i = 0; i < n - 1; i++) {
            consputc(LEFT_ARROW);
          }
          input.e++;
          consputc(input.e == input.rightmost ? ' ' : input.buf[input.e]);
          consputc(LEFT_ARROW);
        }
        //ASAF need to fix cruzer control when more the 12 are entered
        //ASAF needs to make sure enter is enterd when getting to 128 chars in a command...

         /* if (input.e < input.rightmost) {                            //GILAD maybe just use somthing like this and make sure consputc will handle RIGHT_ARROW
            input.e++;
            consputc(c);   // consputx gets a right-arrow, and just move the blinker right
          }*/
        break;


            case UP_ARROW:
                //earase the current line on screen
               // if (historyId == -1) copyCharsToBeMovedferToOldBuf
               //earse current input.buf                                                                                              //DELETE
                //history(current_history_viewed.buf, ++historyId);   call this only if c<15
               // copy current_history_viewed.buf  to screen using "void copy_buffer_to_screen"
              // copy  current_history_viewed.buf to input.buf (doing extrawork when going through history)
             if (current_history_viewed.historyId<15){
                earaseCurrentLineOnScreen();
                if (current_history_viewed.historyId == -1)
                    copyCharsToBeMovedfToOldBuf();
                earaseContentOnInput_buf();
                current_history_viewed.historyId++;

                history(current_history_viewed.buf, current_history_viewed, &current_history_viewed.length);                                                  //GILAD QUES how to make this syscall?!?!

                copyBufferToScreen(current_history_viewed.buf, current_history_viewed.oldBuf.length);
                copyBufferToInputBuf(current_history_viewed.buf, current_history_viewed.oldBuf.length);
              }
           

        
        break;

            case DOWN_ARROW:
                //earase the current line on screen
               //earse current input.buf                                                                                              //GILAD
               // if (historyId == 0) {copyOldBufToInputBuf ; historyId-- ; copyOldBufToScreen}  very similar t the next two lines
                    //{ copy current_history_viewed.buf  to screen using "void copy_buffer_to_screen"
                      // copy  current_history_viewed.buf to input.buf (doing extrawork when going through history)  }   

              switch(current_history_viewed.historyId){
                case -1:
                  //does nothing
                  break;
                case 0: //get string from old buf
                  earaseCurrentLineOnScreen();
                  copyBufferToInputBuf(current_history_viewed.oldBuf, current_history_viewed.oldBuf.lengthOld);
                  copyBufferToScreen(current_history_viewed.oldBuf, current_history_viewed.oldBuf.lengthOld);
                  current_history_viewed.historyId--;                                                
                  break;

                default:
                  earaseCurrentLineOnScreen();
                  current_history_viewed.historyId--; 

                  history(current_history_viewed.buf, current_history_viewed.historyId, &current_history_viewed.length);                                                  //GILAD QUES how to make this syscall?!?!

                  copyBufferToInputBuf(current_history_viewed.buf current_history_viewed.oldBuf.length);
                  copyBufferToScreen(current_history_viewed.buf, current_history_viewed.oldBuf.length);
                  break;
              }
        
        break;



      case '\n':
      case '\r':                                                    //GILAD QUES whats "/r"
          input.e = input.rightmost;
      default:
        if(c != 0 && input.e-input.r < INPUT_BUF){
          c = (c == '\r') ? '\n' : c;
          if (input.rightmost > input.e) { // caret isn't at the end of the line
            copyCharsToBeMoved();
            input.buf[input.e++ % INPUT_BUF] = c;
            input.rightmost++;
            consputc(c);
            shiftbufright();
          }
          else {
            input.buf[input.e++ % INPUT_BUF] = c;
            input.rightmost = input.e - input.rightmost == 1 ? input.e : input.rightmost;                         //GILAD QUES whats this??
            consputc(c);
          }
          if(c == '\n' || c == C('D') || input.e == input.r+INPUT_BUF){
            input.w = input.e;
            wakeup(&input.r);                                                                             //GILAD QUES why waking up on address???
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

/*
  this method eareases the current line from screen
*/
void
earaseCurrentLineOnScreen(void){
                                                                                                              //TODO
}

/*
  this method copies the chars currently on display (and on Input.buf) to current_history_viewed.oldBuf and save its length on current_history_viewed.lengthOld
*/
void
copyCharsToBeMovedfToOldBuf(void){
                                                                                                             //TODO
}

/*
  this method will print the given buf on the screen
*/
void
copyBufferToScreen(char * bufToPrintOnScreen, uint length){
                                                                                                             //TODO
}

/*
  this method will copy the given buf to Input.buf
  will set the input.e and input.rightmost
*/
void
copyBufferToInputBuf(char * bufToSaveInInput, uint length){
                                                                                                             //TODO
}



/*
  this struct will hold the history buffer array                                                                            
*/
struct {
  char bufferArr[MAX_HISTORY][INPUT_BUF]; //holds the actual command strings -
  uint lengthsArr[MAX_HISTORY]; // this will hold the length of each command string
  uint lastCommandIndex;  //the last command of the history
  uint numOfCommmandsInMem; //number of history commands in mem
} history_buffer_array;




/*
  this method writes the requested command in the buffer (and its length)
*/
void
history(char * buffer, int historyId, int *length)
{
  int indexInArray= (lastCommandIndex+historyId)%MAX_MEMORY_COMMAND_IN_HISTORY;
   memmove(buffer, history_buffer_array.bufferArr[indexInArray], history_buffer_array.lengthsArr[indexInArray]);  
   *length = history_buffer_array.lengthsArr[indexInArray];
}


/*
  this method copies the param buffer to the saved history 
  @param length - length of command to be saved                                                                                 //GILAD QUES who should call this??
*/
void
saveCommandInHistory(char * bufferToSave, int length){
  if (history_buffer_array.numOfCommmandsInMem < MAX_HISTORY)
    history_buffer_array.numOfCommmandsInMem++; //when we get to MAX_HISTORY commands in memory we keep on inserting to the array in a circular mution

  history_buffer_array.lastCommandIndex = (history_buffer_array.lastCommandIndex == 0) ? MAX_HISTORY-1 : history_buffer_array.lastCommandIndex--;// does minus 1 % 16
  memmove(history_buffer_array.bufferArr[buffer_array.lastCommandIndex], bufferToSave, length);
  history_buffer_array.lengthsArr[buffer_array.lastCommandIndex] = length;

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

 
    history_buffer_array.numOfCommmandsInMem=0;
    history_buffer_array.lastCommandIndex=0;

}



/*
current_history_viewed.historyId = -1; //at the beginning no history was asked    GILAD




/
  this struct will hold the current history command view.                                                                                   GILAD
/
struct {
  char buf[INPUT_BUF];//holds the actual command string brought from kernel history
  int historyId;  /if historyId==-1, not in use, if historyId==X (0<=X<=15) - buf holds the string of the X's history command  /
                                          can make hold perviuos "un-entered" command when -1"/
  uint lengthHistory; //maybe not needed
  char oldBuf[INPUT_BUF];// holds the command that began to be entered
  uint lengthOld; 
} current_history_viewed;
//current_history_viewed.historyId = -1; //was moved to consoleinit  







*/