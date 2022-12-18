
#ifndef __hdmitable_h__
#define __hdmitable_h__
#include "hdmitx.h"

static const unsigned char HDMI_VIDEO_ID_CODE[HDMI_VIDEO_RESOLUTION_NUM] = {
2, 17, 4, 19, 5, 20, 34, 33, 32, 32, 34, 16, 31 };
/* , , 480P,576P, ,, , ,720P60,720P50,1080I60,1080I50,,,1080P30,1080P25, */
/*  1080P24, 1080P23.97, 1080P29.97, 1080p60,1080p50 */

static const unsigned char PREDIV[3][4] = {
	{0x0, 0x0, 0x0, 0x0},	/* 27Mhz */
	{0x1, 0x1, 0x1, 0x1},	/* 74Mhz */
	{0x1, 0x1, 0x1, 0x1}	/* 148Mhz */
};

static const unsigned char TXDIV[3][4] = {
	{0x3, 0x3, 0x3, 0x2},	/* 27Mhz */
	{0x2, 0x1, 0x1, 0x1},	/* 74Mhz */
	{0x1, 0x0, 0x0, 0x0}	/* 148Mhz */
};

static const unsigned char FBKSEL[3][4] = {
	{0x1, 0x1, 0x1, 0x1},	/* 27Mhz */
	{0x1, 0x0, 0x1, 0x1},	/* 74Mhz */
	{0x1, 0x0, 0x1, 0x1}	/* 148Mhz */
};

static const unsigned char FBKDIV[3][4] = {
	{19, 24, 29, 19},	/* 27Mhz */
	{19, 24, 14, 19},	/* 74Mhz */
	{19, 24, 14, 19}	/* 148Mhz */
};

static const unsigned char DIVEN[3][4] = {
	{0x2, 0x1, 0x1, 0x2},	/* 27Mhz */
	{0x2, 0x2, 0x2, 0x2},	/* 74Mhz */
	{0x2, 0x2, 0x2, 0x2}	/* 148Mhz */
};

static const unsigned char HTPLLBP[3][4] = {
	{0xc, 0xc, 0x8, 0xc},	/* 27Mhz */
	{0xc, 0xf, 0xf, 0xc},	/* 74Mhz */
	{0xc, 0xf, 0xf, 0xc}	/* 148Mhz */
};

static const unsigned char HTPLLBC[3][4] = {
	{0x2, 0x3, 0x3, 0x2},	/* 27Mhz */
	{0x2, 0x3, 0x3, 0x2},	/* 74Mhz */
	{0x2, 0x3, 0x3, 0x2}	/* 148Mhz */
};

static const unsigned char HTPLLBR[3][4] = {
	{0x1, 0x1, 0x0, 0x1},	/* 27Mhz */
	{0x1, 0x2, 0x2, 0x1},	/* 74Mhz */
	{0x1, 0x2, 0x2, 0x1}	/* 148Mhz */
};


#define NCTS_BYTES          0x07
static const unsigned char HDMI_NCTS[7][9][NCTS_BYTES] = {
	{{0x00, 0x00, 0x69, 0x78, 0x00, 0x10, 0x00},	/* 32K, 480i/576i/480p@27MHz/576p@27MHz */
	 {0x00, 0x00, 0xd2, 0xf0, 0x00, 0x10, 0x00},	/* 32K, 480p@54MHz/576p@54MHz */
	 {0x00, 0x03, 0x37, 0xf9, 0x00, 0x2d, 0x80},	/* 32K, 720p@60/1080i@60 */
	 {0x00, 0x01, 0x22, 0x0a, 0x00, 0x10, 0x00},	/* 32K, 720p@50/1080i@50 */
	 {0x00, 0x06, 0x6f, 0xf3, 0x00, 0x2d, 0x80},	/* 32K, 1080p@60 */
	 {0x00, 0x02, 0x44, 0x14, 0x00, 0x10, 0x00},	/* 32K, 1080p@50 */
	 {0x00, 0x01, 0xA5, 0xe0, 0x00, 0x10, 0x00},	/* 32K, 480p@108MHz/576p@108MHz */
	 {0x00, 0x06, 0x6F, 0xF3, 0x00, 0x16, 0xC0},	/* 32K, 296.976m,4K2K */
	 {0x00, 0x03, 0x66, 0x1E, 0x00, 0x0C, 0x00}	/* 32K, 297m */
	 },
	{{0x00, 0x00, 0x75, 0x30, 0x00, 0x18, 0x80},	/* 44K, 480i/576i/480p@27MHz/576p@27MHz */
	 {0x00, 0x00, 0xea, 0x60, 0x00, 0x18, 0x80},	/* 44K, 480p@54MHz/576p@54MHz */
	 {0x00, 0x03, 0x93, 0x87, 0x00, 0x45, 0xac},	/* 44K, 720p@60/1080i@60 */
	 {0x00, 0x01, 0x42, 0x44, 0x00, 0x18, 0x80},	/* 44K, 720p@50/1080i@50 */
	 {0x00, 0x03, 0x93, 0x87, 0x00, 0x22, 0xd6},	/* 44K, 1080p@60 */
	 {0x00, 0x02, 0x84, 0x88, 0x00, 0x18, 0x80},	/* 44K, 1080p@50 */
	 {0x00, 0x01, 0xd4, 0xc0, 0x00, 0x18, 0x80},	/* 44K, 480p@108MHz/576p@108MHz */
	 {0x00, 0x03, 0x93, 0x87, 0x00, 0x11, 0x6B},	/* 44K, 296.976m,4K2K */
	 {0x00, 0x03, 0xC6, 0xCC, 0x00, 0x12, 0x60}	/* 44K, 297m */
	 },
	{{0x00, 0x00, 0x69, 0x78, 0x00, 0x18, 0x00},	/* 48K, 480i/576i/480p@27MHz/576p@27MHz */
	 {0x00, 0x00, 0xd2, 0xf0, 0x00, 0x18, 0x00},	/* 48K, 480p@54MHz/576p@54MHz */
	 {0x00, 0x02, 0x25, 0x51, 0x00, 0x2d, 0x80},	/* 48K, 720p@60/1080i@60 */
	 {0x00, 0x01, 0x22, 0x0a, 0x00, 0x18, 0x00},	/* 48K, 720p@50/1080i@50 */
	 {0x00, 0x02, 0x25, 0x51, 0x00, 0x16, 0xc0},	/* 48K, 1080p@60 */
	 {0x00, 0x02, 0x44, 0x14, 0x00, 0x18, 0x00},	/* 48K, 1080p@50 */
	 {0x00, 0x01, 0xA5, 0xe0, 0x00, 0x18, 0x00},	/* 48K, 108p@54MHz/576p@108MHz */
	 {0x00, 0x04, 0x4A, 0xA2, 0x00, 0x16, 0xC0},	/* 48K, 296.976m,4K2K */
	 {0x00, 0x03, 0xC6, 0xCC, 0x00, 0x14, 0x00}	/* 48K, 297m */
	 },
	{{0x00, 0x00, 0x75, 0x30, 0x00, 0x31, 0x00},	/* 88K 480i/576i/480p@27MHz/576p@27MHz */
	 {0x00, 0x00, 0xea, 0x60, 0x00, 0x31, 0x00},	/* 88K 480p@54MHz/576p@54MHz */
	 {0x00, 0x03, 0x93, 0x87, 0x00, 0x8b, 0x58},	/* 88K, 720p@60/1080i@60 */
	 {0x00, 0x01, 0x42, 0x44, 0x00, 0x31, 0x00},	/* 88K, 720p@50/1080i@50 */
	 {0x00, 0x03, 0x93, 0x87, 0x00, 0x45, 0xac},	/* 88K, 1080p@60 */
	 {0x00, 0x02, 0x84, 0x88, 0x00, 0x31, 0x00},	/* 88K, 1080p@50 */
	 {0x00, 0x01, 0xd4, 0xc0, 0x00, 0x31, 0x00},	/* 88K 480p@108MHz/576p@108MHz */
	 {0x00, 0x03, 0x93, 0x87, 0x00, 0x22, 0xD6},	/* 88K, 296.976m,4K2K */
	 {0x00, 0x03, 0xC6, 0xCC, 0x00, 0x24, 0xC0}	/* 88K, 297m */
	 },
	{{0x00, 0x00, 0x69, 0x78, 0x00, 0x30, 0x00},	/* 96K, 480i/576i/480p@27MHz/576p@27MHz */
	 {0x00, 0x00, 0xd2, 0xf0, 0x00, 0x30, 0x00},	/* 96K, 480p@54MHz/576p@54MHz */
	 {0x00, 0x02, 0x25, 0x51, 0x00, 0x5b, 0x00},	/* 96K, 720p@60/1080i@60 */
	 {0x00, 0x01, 0x22, 0x0a, 0x00, 0x30, 0x00},	/* 96K, 720p@50/1080i@50 */
	 {0x00, 0x02, 0x25, 0x51, 0x00, 0x2d, 0x80},	/* 96K, 1080p@60 */
	 {0x00, 0x02, 0x44, 0x14, 0x00, 0x30, 0x00},	/* 96K, 1080p@50 */
	 {0x00, 0x01, 0xA5, 0xe0, 0x00, 0x30, 0x00},	/* 96K, 480p@108MHz/576p@108MHz */
	 {0x00, 0x04, 0x4A, 0xA2, 0x00, 0x2D, 0x80},	/* 96K, 296.976m,4K2K */
	 {0x00, 0x03, 0xC6, 0xCC, 0x00, 0x28, 0x80}	/* 96K, 297m */
	 },
	{{0x00, 0x00, 0x75, 0x30, 0x00, 0x62, 0x00},	/* 176K, 480i/576i/480p@27MHz/576p@27MHz */
	 {0x00, 0x00, 0xea, 0x60, 0x00, 0x62, 0x00},	/* 176K, 480p@54MHz/576p@54MHz */
	 {0x00, 0x03, 0x93, 0x87, 0x01, 0x16, 0xb0},	/* 176K, 720p@60/1080i@60 */
	 {0x00, 0x01, 0x42, 0x44, 0x00, 0x62, 0x00},	/* 176K, 720p@50/1080i@50 */
	 {0x00, 0x03, 0x93, 0x87, 0x00, 0x8b, 0x58},	/* 176K, 1080p@60 */
	 {0x00, 0x02, 0x84, 0x88, 0x00, 0x62, 0x00},	/* 176K, 1080p@50 */
	 {0x00, 0x01, 0xd4, 0xc0, 0x00, 0x62, 0x00},	/* 176K, 480p@54MHz/576p@54MHz */
	 {0x00, 0x03, 0x93, 0x87, 0x00, 0x45, 0xAC},	/* 176K, 296.976m,4K2K */
	 {0x00, 0x03, 0xC6, 0xCC, 0x00, 0x49, 0x80}	/* 176K, 297m */
	 },
	{{0x00, 0x00, 0x69, 0x78, 0x00, 0x60, 0x00},	/* 192K, 480i/576i/480p@27MHz/576p@27MHz */
	 {0x00, 0x00, 0xd2, 0xf0, 0x00, 0x60, 0x00},	/* 192K, 480p@54MHz/576p@54MHz */
	 {0x00, 0x02, 0x25, 0x51, 0x00, 0xb6, 0x00},	/* 192K, 720p@60/1080i@60 */
	 {0x00, 0x01, 0x22, 0x0a, 0x00, 0x60, 0x00},	/* 192K, 720p@50/1080i@50 */
	 {0x00, 0x02, 0x25, 0x51, 0x00, 0x5b, 0x00},	/* 192K, 1080p@60 */
	 {0x00, 0x02, 0x44, 0x14, 0x00, 0x60, 0x00},	/* 192K, 1080p@50 */
	 {0x00, 0x01, 0xA5, 0xe0, 0x00, 0x60, 0x00},	/* 192K, 480p@108MHz/576p@108MHz */
	 {0x00, 0x04, 0x4A, 0xA2, 0x00, 0x5B, 0x00},	/* 192K, 296.976m,4K2K */
	 {0x00, 0x03, 0xC6, 0xCC, 0x00, 0x50, 0x00}	/* 192K, 297m */

	 }
};



/* ///////////////////////////////////////////////////////// */

#endif
