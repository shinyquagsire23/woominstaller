/*
 *  woomïnstaller - Homebrew package installer for Wii U
 *
 *  Copyright (C) 2016          SALT
 *  Copyright (C) 2016          Max Thomas (Shiny Quagsire) <mtinc2@gmail.com>
 *
 *  This code is licensed under the terms of the GNU LGPL, version 2.1
 *  see file LICENSE.md or https://www.gnu.org/licenses/lgpl-2.1.txt
 */

#ifndef DRAW_H
#define DRAW_H
#include <coreinit/screen.h>
#include <wut_types.h>

typedef struct tga_hdr tga_hdr;
struct __attribute__((__packed__)) tga_hdr
{
    u8 idlength;
    u8 colormaptype;
    u8 datatype;
    u16 colormaporigin;
    u16 colormaplength;
    u8 colormapdepth;
    u16 x_origin;
    u16 y_origin;
    u16 width;
    u16 height;
    u8 bpp;
    u8 imagedescriptor;
};

void *screenBufferTop;
void *screenBufferBottom;

#define SCREEN_TOP 0
#define SCREEN_BOTTOM 1

//Function declarations for my graphics library
void setActiveScreen(int screen);
void flipBuffers();
void fillScreen(char r, char g, char b, char a);
void drawString(int x, int y, char * string);
void drawPixel(int x, int y, char r, char g, char b, char a);
void drawLine(int x1, int y1, int x2, int y2, char r, char g, char b, char a);
void drawBorder(int thickness, char r, char g, char b, char a);
void drawRect(int x1, int y1, int x2, int y2, char r, char g, char b, char a);
void drawRectThickness(int x1, int y1, int x2, int y2, int thickness, char r, char g, char b, char a);
void drawFillRect(int x1, int y1, int x2, int y2, char r, char g, char b, char a);
void drawCircle(int xCen, int yCen, int radius, char r, char g, char b, char a);
void drawFillCircle(int xCen, int yCen, int radius, char r, char g, char b, char a);
void drawCircleCircum(int cx, int cy, int x, int y, char r, char g, char b, char a);
void drawTGA(int x, int y, void *tga_mem);
#endif /* DRAW_H */
