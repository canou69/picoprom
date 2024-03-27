/* Basic XMODEM implementation for Raspberry Pi Pico
 *
 * This only implements the old-school checksum, not CRC, 
 * and only supports 128-byte blocks. But the whole point
 * of XMODEM was to be simple, and this works fine for ~32K
 * transfers over USB-serial.
 */


#include "xmodem.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"


const int XMODEM_SOH = 1;
const int XMODEM_EOT = 4;
const int XMODEM_ACK = 6;
const int XMODEM_BS = 8;
const int XMODEM_DLE = 0x10;
const int XMODEM_NAK = 0x15;
const int XMODEM_CAN = 0x18;
const int XMODEM_SUB = 0x1a;

const int XMODEM_BLOCKSIZE = 128;


xmodem_config_t xmodem_config =
{
	1,    /* logLevel */
	true, /* useCrc */
	false /* useEscape */
};


static char gLogBuffer[65536];
static int gLogPos = 0;


static void xmodem_log(char *s)
{
	if (gLogPos + strlen(s) + 3 >= sizeof gLogBuffer)
	{
		gLogPos = sizeof gLogBuffer;
		return;
	}
	strcpy(gLogBuffer + gLogPos, s);
	gLogPos += strlen(s);
	gLogBuffer[gLogPos++] = '\r';
	gLogBuffer[gLogPos++] = '\n';
	gLogBuffer[gLogPos] = 0;
}

void xmodem_dumplog()
{
	if (gLogPos)
	{
		puts(gLogBuffer);
	}
}

static void xmodem_clearlog()
{
	gLogPos = 0;
	gLogBuffer[gLogPos] = 0;
}


void xmodem_set_config(xmodem_mode_t mode)
{
	bzero(&xmodem_config, sizeof xmodem_config);

	switch (mode)
	{
		case XMODEM_MODE_ORIGINAL:
			xmodem_config.useEscape = false;
			xmodem_config.useCrc = false;
			break;

		case XMODEM_MODE_CRC:
			xmodem_config.useEscape = false;
			xmodem_config.useCrc = true;
			break;
	}
}


int xmodem_receive(void* outputBuffer, size_t bufferSize, const char* message, bool (*inputhandler)())
{
	char logBuffer[1024];
	xmodem_clearlog();

	int sizeReceived = 0;
	int packetNumber = 1;

	bool eof = false;
	bool can = false;
	bool error = false;

	/* Receive a file */
	while (true)
	{
		absolute_time_t nextPrintTime = get_absolute_time();

		/* Receive next packet */
		while (true)
		{
			if (sizeReceived == 0 && absolute_time_diff_us(nextPrintTime, get_absolute_time()) > 0)
			{
				xmodem_dumplog();

				if (message) puts(message);
				
				if (xmodem_config.useCrc)
				{
					putchar(8);
					putchar('C');
				}
				else
				{
					putchar(XMODEM_NAK);
				}

				nextPrintTime = make_timeout_time_ms(3000);
			}

			int c = getchar_timeout_us(1000);
			if (c == PICO_ERROR_TIMEOUT) continue;

			if (c == XMODEM_EOT || c == XMODEM_SOH || c == XMODEM_CAN)
			{
				eof = (c == XMODEM_EOT);
				can = (c == XMODEM_CAN);
				break;
			}
			else if (inputhandler && inputhandler(c))
			{
				return 0;
			}
			else if (c != XMODEM_BS && c != XMODEM_NAK && xmodem_config.logLevel >= 1)
			{
				sprintf(logBuffer, "Unexpected character %d received - expected SOH or EOT", c);
				xmodem_log(logBuffer);
			}
		}

		if (eof) 
		{
			if (xmodem_config.logLevel >= 2) xmodem_log("EOT => ACK");
			putchar(XMODEM_ACK);
			break;
		}

		if (can)
		{
			if (xmodem_config.logLevel >= 1) xmodem_log("CAN => ACK");
			putchar(XMODEM_ACK);
			break;
		}


		if (xmodem_config.logLevel >= 2)
		{
			sprintf(logBuffer, "Got SOH for packet %d", packetNumber);
			xmodem_log(logBuffer);
		}

		if (sizeReceived + XMODEM_BLOCKSIZE > bufferSize)
		{
			error = true;
			xmodem_log("Output buffer full");
			for (int i = 0; i < 8; ++i)
				putchar(XMODEM_CAN);
			while (getchar_timeout_us(1000) != PICO_ERROR_TIMEOUT);
			break;
		}


		bool timeout = false;
		absolute_time_t timeoutTime = make_timeout_time_ms(1000);

		int checksum = 0;
		bool escape = false;

		char buffer[2+XMODEM_BLOCKSIZE+2];
		int bufpos = 0;
		while (bufpos < 2+XMODEM_BLOCKSIZE + (xmodem_config.useCrc ? 2 : 1) && !timeout)
		{
			if (absolute_time_diff_us(timeoutTime, get_absolute_time()) > 0)
			{
				if (xmodem_config.logLevel >= 1) xmodem_log("Timeout");
				timeout = true;
				break;
			}

			int c = getchar_timeout_us(1000);
			if (c == PICO_ERROR_TIMEOUT) continue;

			
			if (xmodem_config.logLevel >= 3)
			{
				sprintf(logBuffer, "Got %d", c);
				xmodem_log(logBuffer);
			}

			bool isData = (bufpos >= 2) && (bufpos < 2+XMODEM_BLOCKSIZE);

			if (xmodem_config.useEscape && isData && c == XMODEM_DLE)
			{
				escape = true;
				continue;
			}

			if (escape) c ^= 0x40;
			escape = false;

			buffer[bufpos++] = c;

			if (isData)
			{
				if (xmodem_config.useCrc)
				{
					checksum = checksum ^ (int)c << 8;
					for (int i = 0; i < 8; ++i)
						if (checksum & 0x8000)
							checksum = checksum << 1 ^ 0x1021;
						else
							checksum = checksum << 1;
				}
				else
				{
					checksum += c;
				}
			}
		}

		bool wrongPacket = (buffer[0] != (char)packetNumber);
		bool badPacketInv = (buffer[1] != (char)(255-buffer[0]));
		bool badChecksum = (buffer[2+XMODEM_BLOCKSIZE] != (char)checksum);
		if (xmodem_config.useCrc)
		{
			badChecksum = (buffer[2+XMODEM_BLOCKSIZE] != (char)(checksum>>8))
				|| (buffer[2+XMODEM_BLOCKSIZE+1] != (char)checksum);
		}

		if (timeout || wrongPacket || badPacketInv || badChecksum)
		{
			if (xmodem_config.logLevel >= 1) xmodem_log("NAK");
			putchar(XMODEM_NAK);
			continue;
		}

		if (xmodem_config.logLevel >= 2) xmodem_log("ACK");
		putchar(XMODEM_ACK);

		memcpy(outputBuffer+sizeReceived, buffer+2, XMODEM_BLOCKSIZE);

		sizeReceived += XMODEM_BLOCKSIZE;
		packetNumber++;
	}

	puts("");
	xmodem_dumplog();
	xmodem_clearlog();

	if (can || error) return -1;

	return sizeReceived;
}

bool xmodem_send(char* inputBuffer, size_t bufferSize)
{
	char logBuffer[1024];
	xmodem_clearlog();
	
	bool result = false;
	int c = 0;
	int block = 1;
	size_t bufpos = 0;
	int checksum = 0;
	int tries = 1;
	bool useCrc = xmodem_config.useCrc;

	// Wait for receive to be ready (30 seconds)
	do
	{
		c = getchar_timeout_us(1000); // 1ms
		if (c != PICO_ERROR_TIMEOUT)
		{
			if (c == 'C') {
				useCrc = true;
				result = true;
				if (xmodem_config.logLevel >= 1) xmodem_log("CRC enabled");
				break;
			}
			else if (c == XMODEM_NAK)
			{
				useCrc = false;
				result = true;
				if (xmodem_config.logLevel >= 1) xmodem_log("CRC disabled");
				break;
			}
			else if (c == XMODEM_BS)
			{
				// Ignore backspaces
				continue;
			}
			else if (xmodem_config.logLevel >= 1)
			{
				sprintf(logBuffer, "Unexpected character %d received - expected %d or %d", c, 'C', XMODEM_NAK);
				xmodem_log(logBuffer);
				xmodem_dumplog();
			}
		}
	}
	while (tries++ < 30000);
	if (tries >= 30000 && xmodem_config.logLevel >= 1) xmodem_log("Timeout");

	while (!!result && bufpos < bufferSize)
	{
		if (xmodem_config.logLevel >= 2) {
			sprintf(logBuffer, "Sending block %d - %d", block, (size_t)block*XMODEM_BLOCKSIZE);
			xmodem_log(logBuffer);
		}

		// Block header
		putchar(XMODEM_SOH);
		putchar((char)block);
		putchar(255-(char)block);

		checksum = 0;
		for (bufpos = (size_t)block*XMODEM_BLOCKSIZE; bufpos < (size_t)(block+1)*XMODEM_BLOCKSIZE; bufpos++)
		{
			c = bufpos < bufferSize ? inputBuffer[bufpos] : XMODEM_SUB;
			putchar(c);

			if (useCrc)
			{
				checksum = checksum ^ c << 8;
				for (int i = 0; i < 8; ++i)
					if (checksum & 0x8000)
						checksum = checksum << 1 ^ 0x1021;
					else
						checksum = checksum << 1;
			}
			else
			{
				checksum += (char)c;
			}
		}
		if (useCrc)
		{
			putchar((char)(checksum>>8));
			putchar((char)checksum);
		}
		else
		{
			putchar((char)checksum);
		}
		if (xmodem_config.logLevel >= 2) {
			sprintf(logBuffer, "Checksum for block %d - %d", block, (useCrc ? checksum & 0xffff : checksum & 0xff));
			xmodem_log(logBuffer);
		}

		c = getchar_timeout_us(1000);
		if (c == XMODEM_ACK)
		{
			block++;
			tries = 0;
		}
		else if (c == XMODEM_CAN && getchar_timeout_us(1000) == XMODEM_CAN)
		{
			result = false;
			break;
		}
		else if (c != XMODEM_NAK && xmodem_config.logLevel >= 2)
		{
			sprintf(logBuffer, "Unknown response %d, retrying block %d", c, block);
			xmodem_log(logBuffer);
		}
		else if (xmodem_config.logLevel >= 2)
		{
			sprintf(logBuffer, "Retrying block %d", block);
			xmodem_log(logBuffer);
		}
		tries++;
		if (tries > 10)
		{
			result = false;
			if (xmodem_config.logLevel >= 1)
			{
				sprintf(logBuffer, "Failed to deliver block %d", block);
				xmodem_log(logBuffer);
			}
			break;
		}
	}

	if (result)
	{
		// Indicate the end of file
		tries = 0;
		putchar(XMODEM_EOT);
		do
		{
			c = getchar_timeout_us(1000);
			if (c != PICO_ERROR_TIMEOUT) {
				if (c == XMODEM_ACK) {
					break;
				} else if (c == XMODEM_CAN && getchar_timeout_us(1000) == XMODEM_CAN) {
					result = false;
					break;
				} else { // c == XMODEM_NAK
					putchar(XMODEM_EOT);
				}
			}
		} while (result && ++tries < 2000);
		if (tries >= 2000)
		{
			result = false;
			if (xmodem_config.logLevel >= 1) xmodem_log("Timeout");
		}
	}
	else
	{
		// Send cancels to terminate the transaction
		for (int i = 0; i < 8; ++i)
			putchar(XMODEM_CAN);
		while (getchar_timeout_us(1000) != PICO_ERROR_TIMEOUT);
		if (xmodem_config.logLevel >= 1) xmodem_log("Transmission cancelled");
	}

	puts("");
	xmodem_dumplog();
	xmodem_clearlog();

	return result;
}
