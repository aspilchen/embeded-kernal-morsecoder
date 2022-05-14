#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/uaccess.h>

//#error Are we building this?

#define MY_DEVICE_FILE  "morse-code"

/******************************************************
 * LED
 ******************************************************/
#include <linux/leds.h>
DEFINE_LED_TRIGGER(morse_ledTrig);

static short ledIsOn = 0;
static void morse_led_on(void)
{
	if(!ledIsOn) {
		led_trigger_event(morse_ledTrig, LED_FULL);
		ledIsOn = 1;
	}
}

static void morse_led_off(void)
{
	if(ledIsOn) {
		led_trigger_event(morse_ledTrig, LED_OFF);
		ledIsOn = 0;
	}
}

static void led_register(void)
{
	// Setup the trigger's name:
	led_trigger_register_simple("morse-code", &morse_ledTrig);
}

static void led_unregister(void)
{
	// Cleanup
	led_trigger_unregister_simple(morse_ledTrig);
}

/**************************************************************
 * FIFO Support
 *************************************************************/
// Info on the interface:
//    https://www.kernel.org/doc/htmldocs/kernel-api/kfifo.html#idp10765104
// Good example:
//    http://lxr.free-electrons.com/source/samples/kfifo/bytestream-example.c

#include <linux/kfifo.h>
#define FIFO_SIZE 256	// Must be a power of 2.
static DECLARE_KFIFO(morse_fifo, char, FIFO_SIZE);

/******************************************************
 * Morse Encoding Functions
 ******************************************************/
#include "morseCodeLetters.h"
#define DOT_TIME 200
#define DASH_TIME DOT_TIME * 3
#define CHAR_BOUNDARY_SLEEP (DOT_TIME * 3)
#define WORD_BOUNDARY_SLEEP (DOT_TIME * 7)
#define SIZE_ALPHABET 26
#define WHITESPACE_INDEX SIZE_ALPHABET
#define WHITESPACE_ENCODING 0
#define SKIP_ENCODING 0xFFFF
#define BYTE_SIZE 8

static void morse_displayEncoding(const unsigned short encoding, unsigned char *inWord)
{
	unsigned short mask;
	unsigned short bitVal;
	unsigned short count = 0;
	int i = 0;
	const unsigned short nBits = sizeof(encoding) * BYTE_SIZE;

	if(encoding == SKIP_ENCODING) {
		return;
	}

	if(encoding == WHITESPACE_ENCODING) {
		if(*inWord) {
			kfifo_put(&morse_fifo, ' ');
			kfifo_put(&morse_fifo, ' ');
			msleep(WORD_BOUNDARY_SLEEP);
		}
		*inWord = 0;
		morse_led_off();
		return;
	}

	if(*inWord) {
		kfifo_put(&morse_fifo, ' ');
		msleep(CHAR_BOUNDARY_SLEEP);
	}

	*inWord = 1;
	for(i = 0; i < nBits; i++) {
		mask = 1 << (nBits - i - 1);
		bitVal = encoding & mask;

		if(bitVal) {
			count += 1;
			continue;
		}

		if(count == 1) {
			kfifo_put(&morse_fifo, '.');
			morse_led_on();
			msleep(DOT_TIME);
		} else if(count == 3) {
			kfifo_put(&morse_fifo, '-');
			morse_led_on();
			msleep(DASH_TIME);
		} 

		if(ledIsOn) {
			count = 0;
			morse_led_off();
			msleep(DOT_TIME);
		}
	}
}

static unsigned short morse_getEncoding(const short index)
{
	if(index >= 0 && index < SIZE_ALPHABET) {
		return morsecode_codes[index];
	}

	if(index == WHITESPACE_INDEX) {
		return WHITESPACE_ENCODING;
	}

	return SKIP_ENCODING;
}

static short morse_charToIndex(const unsigned char value)
{
	if('A' <= value && value <= 'Z') {
		return value - 'A';
	}

	if('a' <= value && value <= 'z') {
		return value - 'a';
	}

	if(value == ' ' || value == '\n' || value == '\r' || value == '\t') {
		return WHITESPACE_INDEX;
	}

	return -1;
}

static unsigned short morse_encode(const char ch)
{
	short index = morse_charToIndex(ch);
	return morse_getEncoding(index);
}

/******************************************************
 * Callbacks
 ******************************************************/

static ssize_t morse_read(struct file *file,
		char *buff, size_t count, loff_t *ppos)
{
	// int data_idx = (int) *ppos;
	int bytes_read = 0;

	if (kfifo_to_user(&morse_fifo, buff, count, &bytes_read)) {
		printk(KERN_ERR "morsecode::morse_read(), Unable to write to buffer.");
		return -EFAULT;
	}

	return bytes_read;  // # bytes actually read.
}

static ssize_t morse_write(struct file* file, const char *buff,
		size_t count, loff_t* ppos)
{
	int i;
	int end;
	char ch;
	unsigned short encoding;
	unsigned char inWord = 0;

	if(!buff) {
		printk(KERN_ERR "morsecode::morse_write(), Invalid input\n");
		return -EFAULT;
	}

	// Strip whitespace at end
	for(end = count-1; end >= 0; end--) {
		if(copy_from_user(&ch, buff+end, sizeof(ch))) {
			printk(KERN_ERR "morsecode::morse_write() Unable to write to file.\n");
			return -EFAULT;
		}
		encoding = morse_encode(ch);
		if(encoding != WHITESPACE_ENCODING && encoding != SKIP_ENCODING) {
			end += 1;
			break;
		}
	}

	for (i = 0; i < end; i++) {
		if(copy_from_user(&ch, buff+i, sizeof(ch))) {
			printk(KERN_ERR "morsecode::morse_write(), Unable to write to file.\n");
			return -EFAULT;
		}
		encoding = morse_encode(ch);
		morse_displayEncoding(encoding, &inWord);
	}

	kfifo_put(&morse_fifo, '\n');
	*ppos += count;
	return count;
}


/******************************************************
 * Misc support
 ******************************************************/

// Callbacks:  (structure defined in <kernel>/include/linux/fs.h)
struct file_operations morse_fops = {
	.owner    =  THIS_MODULE,
	.write    =  morse_write,
	.read     =  morse_read,
};

// Character Device info for the Kernel:
static struct miscdevice morse_miscdevice = {
		.minor    = MISC_DYNAMIC_MINOR,         // Let the system assign one.
		.name     = MY_DEVICE_FILE,             // /dev/.... file.
		.fops     = &morse_fops                    // Callback functions.
};


/******************************************************
 * Driver initialization and exit:
 ******************************************************/


static int __init morse_init(void)
{
	int ret;
	printk(KERN_INFO "----> morse-code driver init().\n");
	INIT_KFIFO(morse_fifo);
	ret = misc_register(&morse_miscdevice);
	led_register();
	return ret;
}

static void __exit morse_exit(void)
{
	printk(KERN_INFO "<---- morse-code driver exit().\n");
	misc_deregister(&morse_miscdevice);
	led_unregister();
}

module_init(morse_init);
module_exit(morse_exit);

MODULE_AUTHOR("Adam Spilchen");
MODULE_DESCRIPTION("Driver to flash morse code on LEDs.");
MODULE_LICENSE("GPL");