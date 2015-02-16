#include "stk500.h"
#include <osapi.h>
//#include <stdio.h>
#include <stdlib.h>
#include "user_interface.h"
#include "espmissingincludes.h"
#include "driver/uart.h"
#include <mem.h>

#define UART_BUF_SIZE 100
static char uart_buf[UART_BUF_SIZE];
static int buf_pos = 0;

#define TICK_TIME 100
#define TICK_TIMEOUT 10000
#define TICK_MAX (TICK_TIMEOUT / TICK_TIME)
#define SYNC_PAUSE 500
#define SYNC_STEP (SYNC_PAUSE / TICK_TIME)

static void add_char(char c)
{
	uart_buf[buf_pos] = c;
	buf_pos++;
	if(buf_pos >= sizeof(uart_buf)) {
		buf_pos = sizeof(uart_buf) - 1;
		// forget the oldest
		memmove(uart_buf, uart_buf+1, sizeof(uart_buf) - 1);
	}
}

static char get_char()
{
	char rs = -1;
	if(buf_pos > 0) {
		rs =  uart_buf[0];
		memmove(uart_buf, uart_buf+1, sizeof(uart_buf) - 1);
		buf_pos--;
	}
	os_printf("get char : %2X\n", rs);
	return rs;
}

static int count_chars()
{
	return buf_pos;
}

static void clean_chars()
{
	buf_pos = 0;
}

os_event_t recvCharTaskQueue[recvCharTaskQueueLen];

static void ICACHE_FLASH_ATTR recvCharTaskCb(os_event_t *events)
{
	uint8_t temp;

	//add transparent determine
	while(READ_PERI_REG(UART_STATUS(UART0)) & (UART_RXFIFO_CNT << UART_RXFIFO_CNT_S))
	{
		WRITE_PERI_REG(0X60000914, 0x73); //WTD

		temp = READ_PERI_REG(UART_FIFO(UART0)) & 0xFF;
		add_char(temp);
		os_printf("char=%2X\n", temp);

	}
	if(UART_RXFIFO_FULL_INT_ST == (READ_PERI_REG(UART_INT_ST(UART0)) & UART_RXFIFO_FULL_INT_ST)) {
		WRITE_PERI_REG(UART_INT_CLR(UART0), UART_RXFIFO_FULL_INT_CLR);
	} else if(UART_RXFIFO_TOUT_INT_ST == (READ_PERI_REG(UART_INT_ST(UART0)) & UART_RXFIFO_TOUT_INT_ST)) {
		WRITE_PERI_REG(UART_INT_CLR(UART0), UART_RXFIFO_TOUT_INT_CLR);
	}
	ETS_UART_INTR_ENABLE();
}


static ETSTimer delayTimer;
static int buflen = 0;
static char *bufptr = NULL;

static int progState = 0;
static int tick = 0;
static int error = 0;

static int in_sync(char fb, char lb)
{
	int rs = fb == 0x14 && lb == 0x10;
	if(rs) {
		os_printf("in sync\n");
	} else {
		os_printf("OUT of sync: %d %d\n", fb, lb);
	}
	return rs;
}

static void stop_ticking()
{
	os_timer_disarm(&delayTimer);
	os_free(bufptr);
	bufptr = NULL;
	buflen = 0;
}

#define PAGE_SIZE (8*8)
#define VALS_COUNT (PAGE_SIZE * 2 + 8)

static void ICACHE_FLASH_ATTR runProgrammer(void *arg)
{
	int i, j, vals[VALS_COUNT], size;
	static int sync_cnt;
	static int address = 0, bufpos;
	char ok, insync, laddress, haddress, *bp, snum[3];
	static char major, minor, signature[3];
	os_printf("state=%d, tick %d\n", progState, tick);
	tick++;
	if(tick > TICK_MAX) {
		os_printf("stk timeout...\n");
		stop_ticking();
	} else {
		switch(progState) {
			case 0:
				clean_chars();
				sync_cnt = 0;
				os_printf("syncing\n");
				uart_tx_one_char(0x30);
				uart_tx_one_char(0x20);
				progState = 1;
				tick = 0;
				break;
			case 1:
				if(count_chars() >= 2) {
					insync = get_char();
					ok = get_char();
					if(in_sync(insync, ok)) {
						os_printf("synced\n");
						uart_tx_one_char(0x41);
						uart_tx_one_char(0x81);
						uart_tx_one_char(0x20);
						os_printf("receiving MAJOR version\n");
						progState = 2;
						tick = 0;
					} else {
						error = 1;
					}
				} else {
					if(tick % SYNC_STEP == 0) {
						if(sync_cnt < 5) {
							clean_chars();
							sync_cnt++;
							os_printf("syncing\n");
							uart_tx_one_char(0x30);
							uart_tx_one_char(0x20);
						} else {
							os_printf("not connected\n");
							stop_ticking();
						}
					}
				}
				break;
			case 2:
				if(count_chars() >= 3) {
					insync = get_char(); // STK_INSYNC
					major = get_char(); // STK_SW_MJAOR
					ok = get_char(); // STK_OK
					if(in_sync(insync, ok)) {
						uart_tx_one_char(0x41);
						uart_tx_one_char(0x82);
						uart_tx_one_char(0x20);
						os_printf("receiving MINOR version\n");
						progState = 3;
						tick = 0;
					} else {
						error = 1;
					}
				}
				break;
			case 3:
				if(count_chars() >= 3) {
					insync = get_char(); // STK_INSYNC
					minor = get_char(); // STK_SW_MJAOR
					ok = get_char(); // STK_OK
					if(in_sync(insync, ok)) {
						os_printf("bootloader version %d.%d\n", major, minor);
						os_printf("entering prog mode\n");
						uart_tx_one_char(0x50);
						uart_tx_one_char(0x20);
						os_printf("receiving sync ack\n");
						progState = 4;
						tick = 0;
					} else {
						error = 1;
					}
				}
				break;
			case 4:
				if(count_chars() >= 2) {
					insync = get_char();
					ok = get_char();
					if(in_sync(insync, ok)) {
						uart_tx_one_char(0x75);
						uart_tx_one_char(0x20);
						os_printf("receiving signature\n");
						progState = 5;
					} else {
						error = 1;
					}
				}
				break;
			case 5:
				if(count_chars() >= 3) {
					insync = get_char(); // STK_INSYNC
					signature[0] = get_char();
					signature[1] = get_char();
					signature[2] = get_char();
					ok = get_char(); // STK_OK
					if(in_sync(insync, ok)) {
						os_printf("signature %d-%d-%d\n", signature[0], signature[1], signature[2]);
						address = 0;
						progState = 6;
						tick = 0;
					} else {
						error = 1;
					}
				}
				break;
			case 6:
				laddress = address & 0xFF;
				haddress = address >> 8 & 0xff;
				os_printf("set page addr: %4X\n", address);
				bufpos = 0;
				// skip two lines
/*				i = 0;
				while(bufpos < buflen) {
					while(bufpos < buflen && (bufptr[bufpos] == '\x0a' || bufptr[bufpos] == '\x0d')) {
						bufpos++;
						i += bufptr[bufpos] == '\x0a';
					}
					if(i>1) {
						break;
					}
					bufpos++;
				}
*/				address += PAGE_SIZE;
				uart_tx_one_char(0x55); //STK_LOAD_ADDRESS
				uart_tx_one_char(laddress);
				uart_tx_one_char(haddress);
				uart_tx_one_char(0x20); // SYNC_CRC_EOP
				progState = 7;
				tick = 0;
				break;
			case 7:
				if(count_chars() >= 2) {
					insync = get_char();
					ok = get_char();
					if(in_sync(insync, ok)) {
						os_printf("sending program page <=%d bytes\n", PAGE_SIZE);
						memset(vals, 0, sizeof(vals));
						size = 0;
						for(j=0; j<8; j++) {
							// process single line
							// skip till ':'
							while(bufpos < buflen-1) {
								if(bufptr[bufpos++] == ':') {
									break;
								}
							}
							// skip 8 byte of address
							bufpos += 8;
							for(i=0; i<16; i++) {
								if(bufpos >= buflen) {
									// if no more buffer
									break;
								}
								bp = bufptr + bufpos;
								if(*bp == '\x0a' || *bp == '\x0d') {
									// premature end of line
									// the last byte is not needed
									size--;
									break;
								}
								snum[0] = bp[0];
								snum[1] = bp[1];
								snum[2] = 0;
								vals[size++] = (int)strtol(snum, NULL, 16);
								os_printf("j=%d, i=%d, size=%d, b=%2X (%s)\n", j, i, size, vals[size-1], snum);
								bufpos += 2;
							}
						}
						uart_tx_one_char(0x64); // STK_PROGRAM_PAGE
						uart_tx_one_char(0); // page size
						uart_tx_one_char(size); // page size
						uart_tx_one_char(0x46); // flash memory, 'F'
						os_printf("size=%d\n", size);
						for(j=0; j<size; j++) {
							uart_tx_one_char(vals[j]);
						}
						uart_tx_one_char(0x20); // SYNC_CRC_EOP
						if(bufpos >= buflen) {
							// end of buffer - stop/go to the next stage
							progState = 8;
						}
						tick = 0;
					} else {
						error = 1;
					}
				}
				break;
			case 8:
				if(count_chars() >= 2) {
					insync = get_char();
					ok = get_char();
					if(in_sync(insync, ok)) {
						os_printf("leaving prog mode\n");
						uart_tx_one_char(0x51);
						uart_tx_one_char(0x20);
						progState = 9;
						tick = 0;
					} else {
						error = 1;
					}
				}
				break;
			case 9:
				if(count_chars() >= 2) {
					insync = get_char();
					ok = get_char();
					if(in_sync(insync, ok)) {
						os_printf("end\n");
						stop_ticking();
					} else {
						error = 1;
					}
				}
				break;
		}
	}
	if(error) {
		os_printf("Error occured\n");
		stop_ticking();
	}
}

void program(int size, char *buf)
{
	os_timer_disarm(&delayTimer);
	if(bufptr != NULL) {
		// clean old buffer
		os_free(bufptr);
	}
	bufptr = buf;
	buflen = size;
	progState = 0;
	tick = 0;
	error = 0;
	os_timer_setfn(&delayTimer, runProgrammer, NULL);
	os_timer_arm(&delayTimer, 100, 1);
}

void init_stk500()
{
	system_os_task(recvCharTaskCb, recvCharTaskPrio, recvCharTaskQueue, recvCharTaskQueueLen);
}