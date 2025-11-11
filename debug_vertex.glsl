#version 330
uniform float BLURSCALEX;
uniform float BRIGHTBOOST;
uniform float HILUMSCAN;
uniform float LOWLUMSCAN;
uniform float MASK_DARK;
uniform float MASK_FADE;

// This can't be an option without slowing the shader down.
// Note that only the fine mask works on SNES Classic Edition
// due to Mali 400 gpu precision.
#define FINEMASK

/*
    zfast_crt_standard - A simple, fast CRT shader.

    Copyright (C) 2017 Greg Hogan (SoltanGris42)

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation; either version 2 of the License, or (at your option)
    any later version.

Notes:  This shader does scaling with a weighted linear filter for adjustable
	sharpness on the x and y axes based on the algorithm by Inigo Quilez here:
	http://http://www.iquilezles.org/www/articles/texture/texture.htm
	but modified to be somewhat sharper.  Then a scanline effect that varies
	based on pixel brighness is applied along with a monochrome aperture mask.
	This shader runs at 60fps on the Raspberry Pi 3 hardware at 2mpix/s
	resolutions (1920x1080 or 1600x1200).
*/

uniform vec4 SourceSize;
uniform vec4 OriginalSize;
uniform vec4 OutputSize;
uniform float FrameCount;

#pragma parameter BLURSCALEX "Blur Amount X-Axis" 0.30 0.0 1.0 0.05
#pragma parameter LOWLUMSCAN "Scanline Darkness - Low" 6.0 0.0 10.0 0.5
#pragma parameter HILUMSCAN "Scanline Darkness - High" 8.0 0.0 50.0 1.0
#pragma parameter BRIGHTBOOST "Dark Pixel Brightness Boost" 1.25 0.5 1.5 0.05
#pragma parameter MASK_DARK "Mask Effect Amount" 0.25 0.0 1.0 0.05
#pragma parameter MASK_FADE "Mask/Scanline Fade" 0.8 0.0 1.0 0.05

//For testing compilation 
//#define FRAGMENT
//#define VERTEX

//Some drivers don't return black with texture coordinates out of bounds
//SNES Classic is too slow to black these areas out when using fullscreen
//overlays.  But you can uncomment the below to black them out if necessary
//#define BLACK_OUT_BORDER

in vec4 Position;
in vec2 TexCoord;
out vec2 vTexCoord;
out float maskFade;
out vec2 invDims;

void main()
{
   gl_Position = Position;
   vTexCoord = TexCoord;
	maskFade = 0.3333*MASK_FADE;
	invDims = 1.0/SourceSize.xy;
}

