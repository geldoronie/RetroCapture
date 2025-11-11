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

in vec2 vTexCoord;
in float maskFade;
in vec2 invDims;
out vec4 FragColor;
 uniform sampler2D Source;

void main()
{
	//This is just like "Quilez Scaling" but sharper
	vec2 p = vTexCoord * SourceSize.xy;
	vec2 i = floor(p) + 0.50;
	vec2 f = p - i;
	p = (i + 4.0*f*f*f)*invDims;
	p.x = mix( p.x , vTexCoord.x, BLURSCALEX);
	float Y = f.y*f.y;
	float YY = Y*Y;
	
#if defined(FINEMASK) 
	float whichmask = fract(floor(vTexCoord.x*OutputSize.x)*-0.4999);
	float mask = 1.0 + float(whichmask < 0.5) * -MASK_DARK;
#else
	float whichmask = fract(floor(vTexCoord.x*OutputSize.x)*-0.3333);
	float mask = 1.0 + float(whichmask <= 0.33333) * -MASK_DARK;
#endif
	vec3 colour = texture(Source, p).rgb;
	
	float scanLineWeight = (BRIGHTBOOST - LOWLUMSCAN*(Y - 2.05*YY));
	float scanLineWeightB = 1.0 - HILUMSCAN*(YY-2.8*YY*Y);	
	
#if defined(BLACK_OUT_BORDER)
	colour.rgb*=float(tc.x > 0.0)*float(tc.y > 0.0); //why doesn't the driver do the right thing?
#endif

	FragColor.rgb = colour.rgb*mix(scanLineWeight*mask, scanLineWeightB, dot(colour.rgb,vec3(maskFade)));
}

