/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// r_surf.c: surface-related refresh code

#include "quakedef.h"

int			skytexturenum;

#ifndef GL_RGBA4
#define	GL_RGBA4	0
#endif


int			lightmap_bytes;		// 1, 2, or 4

#define		ATLAS_WIDTH		8 //in units of BLOCK_WIDTH
#define		ATLAS_HEIGHT	8 //in units of BLOCK_HEIGHT
#define		MAX_LIGHTMAPS	ATLAS_WIDTH * ATLAS_HEIGHT
#define		BLOCK_WIDTH		128
#define		BLOCK_HEIGHT	128

int			lightmap_textures_gl[4] = { 0, 0, 0, 0 };// triple buffered, one atlassed texture
int			lightmap_active_index = 0;
int			lightmapsDataOffets[MAX_LIGHTMAPS];
int			lightmapsGLSOffets[MAX_LIGHTMAPS];
int			lightmapsGLTOffets[MAX_LIGHTMAPS];

unsigned	blocklights[18*18];

int			active_lightmaps;

typedef struct glRect_s {
	unsigned char l,t,w,h;
} glRect_t;

glpoly_t	*lightmap_polys[MAX_LIGHTMAPS];
qboolean	lightmap_modified[MAX_LIGHTMAPS];
int			lightmap_was_modified[MAX_LIGHTMAPS] = {0};
glRect_t	lightmap_rectchange[MAX_LIGHTMAPS];
glRect_t	lightmap_rectchange_cache[MAX_LIGHTMAPS]; //stores the largest recent edit if needed

int			allocated[MAX_LIGHTMAPS][BLOCK_WIDTH];

// the lightmap texture data needs to be kept in
// main memory so texsubimage can update properly
byte		lightmapsData[1*MAX_LIGHTMAPS*BLOCK_WIDTH*BLOCK_HEIGHT];
msurface_t  *skychain = NULL;
msurface_t  *waterchain = NULL;

BatchElements batchElements = {0,0,{{0},{0},{0}}};

void R_RenderDynamicLightmaps (msurface_t *fa);

/*
===============
R_AddDynamicLights
===============
*/
void R_AddDynamicLights (msurface_t *surf)
{
	int			lnum;
	int			sd, td;
	float		dist, rad, minlight;
	vec3_t		impact, local;
	int			s, t;
	int			i;
	int			smax, tmax;
	mtexinfo_t	*tex;

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	tex = surf->texinfo;

	for (lnum=0 ; lnum<MAX_DLIGHTS ; lnum++)
	{
		if ( !(surf->dlightbits & (1<<lnum) ) )
			continue;		// not lit by this light

		rad = cl_dlights[lnum].radius;
		dist = DotProduct (cl_dlights[lnum].origin, surf->plane->normal) -
				surf->plane->dist;
		rad -= fabs(dist);
		minlight = cl_dlights[lnum].minlight;
		if (rad < minlight)
			continue;
		minlight = rad - minlight;

		for (i=0 ; i<3 ; i++)
		{
			impact[i] = cl_dlights[lnum].origin[i] -
					surf->plane->normal[i]*dist;
		}

		local[0] = DotProduct (impact, tex->vecs[0]) + tex->vecs[0][3];
		local[1] = DotProduct (impact, tex->vecs[1]) + tex->vecs[1][3];

		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];
		
		for (t = 0 ; t<tmax ; t++)
		{
			td = local[1] - t*16;
			if (td < 0)
				td = -td;
			for (s=0 ; s<smax ; s++)
			{
				sd = local[0] - s*16;
				if (sd < 0)
					sd = -sd;
				if (sd > td)
					dist = sd + (td>>1);
				else
					dist = td + (sd>>1);
				if (dist < minlight)
					blocklights[t*smax + s] += (rad - dist)*256;
			}
		}
	}
}


/*
===============
R_BuildLightMap

Combine and scale multiple lightmaps into the 8.8 format in blocklights
===============
*/
void R_BuildLightMap (msurface_t *surf, byte *dest, int stride)
{
	int			smax, tmax;
	int			t;
	int			i, j, size;
	byte		*lightmap;
	unsigned	scale;
	int			maps;
	int			lightadj[4];
	unsigned	*bl;

	surf->cached_dlight = (surf->dlightframe == r_framecount);

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	size = smax*tmax;
	lightmap = surf->samples;

// set to full bright if no light data
	if (r_fullbright.value || !cl.worldmodel->lightdata)
	{
		for (i=0 ; i<size ; i++)
			blocklights[i] = 255*256;
		goto store;
	}

// clear to no light
	for (i=0 ; i<size ; i++)
		blocklights[i] = 0;

// add all the lightmaps
	if (lightmap)
		for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ;
			 maps++)
		{
			scale = d_lightstylevalue[surf->styles[maps]];
			surf->cached_light[maps] = scale;	// 8.8 fraction
			for (i=0 ; i<size ; i++)
				blocklights[i] += lightmap[i] * scale;
			lightmap += size;	// skip to next lightmap
		}

// add all the dynamic lights
	if (surf->dlightframe == r_framecount)
		R_AddDynamicLights (surf);

// bound, invert, and shift
store:
	switch (gl_lightmap_format)
	{
	case GL_RGBA:
		stride -= (smax<<2);
		bl = blocklights;
		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				t = *bl++;
				t >>= 7;
				if (t > 255)
					t = 255;
				dest[3] = t;
				dest += 4;
			}
		}
		break;
	case GL_RED:	
		bl = blocklights;
		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				t = *bl++;
				t >>= 7;
				if (t > 511)
					t = 511;
				t = (((float)t) / ((float)511)) * 255;
				dest[j] = t;
			}
		}
		break;
	default:
		Sys_Error ("Bad lightmap format");
	}
}


/*
===============
R_TextureAnimation

Returns the proper texture for a given time and base texture
===============
*/
texture_t *R_TextureAnimation (texture_t *base)
{
	int		reletive;
	int		count;

	if (currententity->frame)
	{
		if (base->alternate_anims)
			base = base->alternate_anims;
	}
	
	if (!base->anim_total)
		return base;

	reletive = (int)(cl.time*10) % base->anim_total;

	count = 0;	
	while (base->anim_min > reletive || base->anim_max <= reletive)
	{
		base = base->anim_next;
		if (!base)
			Sys_Error ("R_TextureAnimation: broken cycle");
		if (++count > 100)
			Sys_Error ("R_TextureAnimation: infinite cycle");
	}

	return base;
}


/*
=============================================================

	BRUSH MODELS

=============================================================
*/
extern	float	speedscale;		// for top sky and bottom sky

void DrawGLWaterPoly (glpoly_t *p);
void DrawGLWaterPolyLightmap (glpoly_t *p);

lpMTexFUNC qglMTexCoord2fSGIS = NULL;
lpSelTexFUNC qglSelectTextureSGIS = NULL;

qboolean mtexenabled = false;

void GL_SelectTexture (GLenum target);

void GL_DisableMultitexture(void) 
{
	if (mtexenabled) {
		glDisable(GL_TEXTURE_2D);
		GL_SelectTexture(TEXTURE0_SGIS);
		mtexenabled = false;
	}
}

void GL_EnableMultitexture(void) 
{
	if (gl_mtexable) {
		GL_SelectTexture(TEXTURE1_SGIS);
		glEnable(GL_TEXTURE_2D);
		mtexenabled = true;
	}
}


/*
================
AppendGLPoly
================
*/
void AppendGLPoly (glpoly_t *p)
{
	if(p->vertexOffset < 0)return;
	TEMP_INDEX_BUFFER
	for(i=0;i<(p->numverts-2)*3;i++){
		batchElements.element[batchElements.bufferIndex][batchElements.index++] = buffer[i] + p->vertexOffset;
	}
}

/*
================
DrawGLWaterPoly

Warp the vertex coordinates
================
*/
void DrawGLWaterPoly (glpoly_t *p)
{
	AppendGLPoly(p);
}

void DrawGLWaterPolyLightmap (glpoly_t *p)
{
	AppendGLPoly(p);
}

/*
================
DrawGLPoly
================
*/
void DrawGLPoly ()
{
	if(batchElements.index == 0)return;
	RenderBrushDataElements(batchElements.element[batchElements.bufferIndex], batchElements.index);
	batchElements.bufferIndex = (batchElements.bufferIndex + 1) % MAX_ELEMENT_BUFFERS;
	batchElements.index = 0;
}

/*
================
R_UpdateLightmaps
================
*/
void R_UpdateLightmaps (void)
{
	int			i, j;
	glpoly_t	*p;
	float		*v,*first_vtx;
	glRect_t	*theRect;

	if (r_fullbright.value)
		return;

	if (lightmap_active_index == 0)
	{	glActiveTexture(GL_TEXTURE0 + TEX_SLOT_LIGHT_0);}
	else if (lightmap_active_index == 1)
	{	glActiveTexture(GL_TEXTURE0 + TEX_SLOT_LIGHT_1);}
	else if (lightmap_active_index == 2)
	{	glActiveTexture(GL_TEXTURE0 + TEX_SLOT_LIGHT_2);}
	else
	{	glActiveTexture(GL_TEXTURE0 + TEX_SLOT_LIGHT_3);}


	for (i=0 ; i<MAX_LIGHTMAPS ; i++)
	{
		if (lightmap_modified[i] || lightmap_was_modified[i] >= 0)
		{
			if(lightmap_modified[i])
				lightmap_was_modified[i] = 14 ;
			lightmap_modified[i] = false;

			//if(lightmap_rectchange_cache[i].h == 0){
			//	lightmap_rectchange_cache[i] = lightmap_rectchange[i];
			//}
			//else
			//{
				if(lightmap_rectchange_cache[i].t > lightmap_rectchange[i].t)
					lightmap_rectchange_cache[i].t = lightmap_rectchange[i].t;
				if(lightmap_rectchange_cache[i].h < lightmap_rectchange[i].h)
					lightmap_rectchange_cache[i].h = lightmap_rectchange[i].h;
			//}

			theRect = &lightmap_rectchange_cache[i];

			glTexSubImage2D(GL_TEXTURE_2D, 0, lightmapsGLSOffets[i], lightmapsGLTOffets[i] + theRect->t,
							BLOCK_WIDTH, theRect->h, gl_lightmap_format, GL_UNSIGNED_BYTE,
							lightmapsData + (i* BLOCK_HEIGHT + theRect->t) *BLOCK_WIDTH*lightmap_bytes);
			

			lightmap_rectchange[i].l = BLOCK_WIDTH;
			lightmap_rectchange[i].t = BLOCK_HEIGHT;
			lightmap_rectchange[i].h = 0;
			lightmap_rectchange[i].w = 0;

			if(lightmap_was_modified[i]<= 0){
				lightmap_was_modified[i] = 0;
				lightmap_rectchange_cache[i].l = BLOCK_WIDTH;
				lightmap_rectchange_cache[i].t = BLOCK_HEIGHT;
				lightmap_rectchange_cache[i].h = 0;
				lightmap_rectchange_cache[i].w = 0;
			}
			lightmap_was_modified[i]--;

		}
		//else if(lightmap_was_modified[i] >= 0) //final update fix for double buffering
		//{
		//	lightmap_was_modified[i]--;
		//	theRect = &lightmap_rectchange_cache[i];
		//	glTexSubImage2D(GL_TEXTURE_2D, 0, lightmapsGLSOffets[i], lightmapsGLTOffets[i] + theRect->t,
		//					BLOCK_WIDTH, theRect->h, gl_lightmap_format, GL_UNSIGNED_BYTE,
		//					lightmapsData + (i* BLOCK_HEIGHT + theRect->t) *BLOCK_WIDTH*lightmap_bytes);
		//	//if(lightmap_was_modified[i]<= 0){
		//	//	lightmap_was_modified[i] = 0;
		//	//	lightmap_rectchange_cache[i].l = BLOCK_WIDTH;
		//	//	lightmap_rectchange_cache[i].t = BLOCK_HEIGHT;
		//	//	lightmap_rectchange_cache[i].h = 0;
		//	//	lightmap_rectchange_cache[i].w = 0;
		//	//}
		//}
	}

	lightmap_active_index++;
	if(lightmap_active_index > 3)lightmap_active_index = 0;
}

/*
================
R_RenderBrushPoly
================
*/
void R_RenderBrushPoly (msurface_t *fa)
{
	texture_t	*t;
	byte		*base;
	int			maps;
	glRect_t    *theRect;
	int smax, tmax;


	c_brush_polys++;

	t = R_TextureAnimation (fa->texinfo->texture);
	if(t != fa->texinfo->texture)
	{
		//draw what we have so far and then start again
		DrawGLPoly ();
		GL_BindNoFlush( t->gl_texturenum, TEX_SLOT_CLR );
	}

	if (fa->flags & SURF_DRAWTURB){
		EmitWaterPolys(fa);  return;
	}


	if (fa->flags & SURF_UNDERWATER){
 		AppendGLPoly (fa->polys);
	}
	else{
		AppendGLPoly (fa->polys);
	}

	// add the poly to the proper lightmap chain
	fa->polys->chain = lightmap_polys[fa->lightmaptexturenum];
	lightmap_polys[fa->lightmaptexturenum] = fa->polys;

	// check for lightmap modification
	for (maps = 0 ; maps < MAXLIGHTMAPS && fa->styles[maps] != 255 ;
		 maps++)
		if (d_lightstylevalue[fa->styles[maps]] != fa->cached_light[maps])
			goto dynamic;

	if (fa->dlightframe == r_framecount	// dynamic this frame
		|| fa->cached_dlight)			// dynamic previously
	{
dynamic:
		if (r_dynamic.value)
		{
			lightmap_modified[fa->lightmaptexturenum] = true;
			theRect = &lightmap_rectchange[fa->lightmaptexturenum];
			if (fa->light_t < theRect->t) {
				if (theRect->h)
					theRect->h += theRect->t - fa->light_t;
				theRect->t = fa->light_t;
			}
			if (fa->light_s < theRect->l) {
				if (theRect->w)
					theRect->w += theRect->l - fa->light_s;
				theRect->l = fa->light_s;
			}
			smax = (fa->extents[0]>>4)+1;
			tmax = (fa->extents[1]>>4)+1;
			if ((theRect->w + theRect->l) < (fa->light_s + smax))
				theRect->w = (fa->light_s-theRect->l)+smax;
			if ((theRect->h + theRect->t) < (fa->light_t + tmax))
				theRect->h = (fa->light_t-theRect->t)+tmax;

			//maybe just update the lightmap whoelsale?
			//base = lightmapsData + lightmapsDataOffets[fa->lightmaptexturenum];
			//base += ((fa->light_t * BLOCK_WIDTH*ATLAS_WIDTH) + fa->light_s) * lightmap_bytes;
			//R_BuildLightMap (fa, base, ATLAS_WIDTH* BLOCK_WIDTH*lightmap_bytes);
			base = lightmapsData + fa->lightmaptexturenum*lightmap_bytes*BLOCK_WIDTH*BLOCK_HEIGHT;
			base += fa->light_t * BLOCK_WIDTH * lightmap_bytes + fa->light_s * lightmap_bytes;
			R_BuildLightMap (fa, base, BLOCK_WIDTH*lightmap_bytes);
		}
	}
}


/*
================
R_RenderDynamicLightmaps
Multitexture
================
*/
void R_RenderDynamicLightmaps (msurface_t *fa)
{
	texture_t	*t;
	byte		*base;
	int			maps;
	glRect_t    *theRect;
	int smax, tmax;

	c_brush_polys++;

	if (fa->flags & ( SURF_DRAWSKY | SURF_DRAWTURB) )
		return;
		
	fa->polys->chain = lightmap_polys[fa->lightmaptexturenum];
	lightmap_polys[fa->lightmaptexturenum] = fa->polys;

	// check for lightmap modification
	for (maps = 0 ; maps < MAXLIGHTMAPS && fa->styles[maps] != 255 ;
		 maps++)
		if (d_lightstylevalue[fa->styles[maps]] != fa->cached_light[maps])
			goto dynamic;

	if (fa->dlightframe == r_framecount	// dynamic this frame
		|| fa->cached_dlight)			// dynamic previously
	{
dynamic:
		if (r_dynamic.value)
		{
			lightmap_modified[fa->lightmaptexturenum] = true;

			theRect = &lightmap_rectchange[fa->lightmaptexturenum];
			if (fa->light_t < theRect->t) {
				if (theRect->h)
					theRect->h += theRect->t - fa->light_t;
				theRect->t = fa->light_t;
			}
			if (fa->light_s < theRect->l) {
				if (theRect->w)
					theRect->w += theRect->l - fa->light_s;
				theRect->l = fa->light_s;
			}
			smax = (fa->extents[0]>>4)+1;
			tmax = (fa->extents[1]>>4)+1;
			if ((theRect->w + theRect->l) < (fa->light_s + smax))
				theRect->w = (fa->light_s-theRect->l)+smax;
			if ((theRect->h + theRect->t) < (fa->light_t + tmax))
				theRect->h = (fa->light_t-theRect->t)+tmax;
			//maybe just update the lightmap whoelsale?
			//base = lightmapsData + lightmapsDataOffets[fa->lightmaptexturenum];
			//base += ((fa->light_t * BLOCK_WIDTH*ATLAS_WIDTH) + fa->light_s) * lightmap_bytes;
			//R_BuildLightMap(fa, base, ATLAS_WIDTH* BLOCK_WIDTH*lightmap_bytes);
			base = lightmapsData + fa->lightmaptexturenum*lightmap_bytes*BLOCK_WIDTH*BLOCK_HEIGHT;
			base += fa->light_t * BLOCK_WIDTH * lightmap_bytes + fa->light_s * lightmap_bytes;
			R_BuildLightMap(fa, base, BLOCK_WIDTH*lightmap_bytes);
		}
	}
}

/*
================
R_MirrorChain
================
*/
void R_MirrorChain (msurface_t *s)
{
	if (mirror)
		return;
	mirror = true;
	mirror_plane = s->plane;
}


/*
================
R_DrawWaterSurfaces
================
*/
void R_DrawWaterSurfaces (void)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;

	if (r_wateralpha.value == 1.0 )
		return;

	//
	// go back to the world matrix
	//

	glLoadMatrixf (r_world_matrix);

	if (r_wateralpha.value < 1.0) {
		EnableBlending();
	}

	for (i=0 ; i<cl.worldmodel->numtextures ; i++)
	{
		t = cl.worldmodel->textures[i];
		if (!t)
			continue;
		s = t->texturechain;
		if (!s)
			continue;
		if ( !(s->flags & SURF_DRAWTURB ) )
			continue;

		// set modulate mode explicitly
		GL_BindNoFlush (t->gl_texturenum, TEX_SLOT_CLR);

		for ( ; s ; s=s->texturechain)
			EmitWaterPolys (s);

		DrawGLPoly();
		
		t->texturechain = NULL;
	}

	if (r_wateralpha.value < 1.0) {
		//glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

		//glColor4f (1,1,1,1);
		//glDisable (GL_BLEND);
		AddVertex4D (VTX_COLOUR, 1, 1, 1, 1.0f);	
		DisableBlending();
	}

}


/*
================
DrawTextureChains
================
*/
void DrawTextureChains(int phase) //temporary 0 = normal, 1 = water, 2 = sky
{
	int		i;
	msurface_t	*s;
	texture_t	*t;

	int batchedIndices[1024] = {0};

	for (i=0 ; i<cl.worldmodel->numtextures ; i++)
	{
		t = cl.worldmodel->textures[i];

		if (!t)
			continue;

		s = t->texturechain;
		if (!s)
			continue;
		if (i == skytexturenum)
		{
			if (phase == 2)
				R_DrawSkyChain(s);
			else
				continue;
		}
		else if (i == mirrortexturenum && r_mirroralpha.value != 1.0)
		{
			if (phase == 0){
				GL_BindNoFlush(t->gl_texturenum, TEX_SLOT_CLR);
				R_MirrorChain(s);
				DrawGLPoly();
				continue;
			}
		}
		else
		{
			if (s->flags & SURF_DRAWTURB && phase != 1)
				continue;
			
			GL_BindNoFlush(t->gl_texturenum, TEX_SLOT_CLR);
			for ( ; s ; s=s->texturechain)
			{
				R_RenderBrushPoly(s);
			}
			DrawGLPoly();
		}

		t->texturechain = NULL;
	}
}

/*
=================
R_DrawBrushModel
=================
*/
void R_DrawBrushModel (entity_t *e)
{
	int			j, k;
	vec3_t		mins, maxs;
	int			i, numsurfaces;
	msurface_t	*psurf;
	float		dot;
	mplane_t	*pplane;
	model_t		*clmodel;
	qboolean	rotated;

	currententity = e;
	currenttexture = -1;

	clmodel = e->model;

	if (e->angles[0] || e->angles[1] || e->angles[2])
	{
		rotated = true;
		for (i=0 ; i<3 ; i++)
		{
			mins[i] = e->origin[i] - clmodel->radius;
			maxs[i] = e->origin[i] + clmodel->radius;
		}
	}
	else
	{
		rotated = false;
		VectorAdd (e->origin, clmodel->mins, mins);
		VectorAdd (e->origin, clmodel->maxs, maxs);
	}

	if (R_CullBox (mins, maxs))
		return;

	//glColor3f (1,1,1);
	memset (lightmap_polys, 0, sizeof(lightmap_polys));

	VectorSubtract (r_refdef.vieworg, e->origin, modelorg);
	if (rotated)
	{
		vec3_t	temp;
		vec3_t	forward, right, up;

		VectorCopy (modelorg, temp);
		AngleVectors (e->angles, forward, right, up);
		modelorg[0] = DotProduct (temp, forward);
		modelorg[1] = -DotProduct (temp, right);
		modelorg[2] = DotProduct (temp, up);
	}

	psurf = &clmodel->surfaces[clmodel->firstmodelsurface];

// calculate dynamic lighting for bmodel if it's not an
// instanced model
	if (clmodel->firstmodelsurface != 0 && !gl_flashblend.value)
	{
		for (k=0 ; k<MAX_DLIGHTS ; k++)
		{
			if ((cl_dlights[k].die < cl.time) ||
				(!cl_dlights[k].radius))
				continue;

			R_MarkLights (&cl_dlights[k], 1<<k,
				clmodel->nodes + clmodel->hulls[0].firstclipnode);
		}
	}

	Push();
e->angles[0] = -e->angles[0];	// stupid quake bug
	R_RotateForEntity (e);
e->angles[0] = -e->angles[0];	// stupid quake bug

	UpdateTransformUBOs();
	//
	// draw texture
	//
	int currentTex = psurf->texinfo->texture->gl_texturenum;
	GL_BindNoFlush(currentTex,TEX_SLOT_CLR);
	for (i=0 ; i<clmodel->nummodelsurfaces ; i++, psurf++)
	{
	// find which side of the node we are on
		pplane = psurf->plane;
		dot = DotProduct (modelorg, pplane->normal) - pplane->dist;
	// draw the polygon
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			(!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
			if(currentTex != psurf->texinfo->texture->gl_texturenum)
			{
				DrawGLPoly();
				currentTex = psurf->texinfo->texture->gl_texturenum;
				GL_BindNoFlush(currentTex,TEX_SLOT_CLR);
			}
			R_RenderBrushPoly (psurf);
		}
	}
	DrawGLPoly();

	Pop();
}

/*
=============================================================

	WORLD MODEL

=============================================================
*/

/*
================
R_RecursiveWorldNode
================
*/
void R_RecursiveWorldNode (mnode_t *node)
{
	int			i, c, side, *pindex;
	vec3_t		acceptpt, rejectpt;
	mplane_t	*plane;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf;
	double		d, dot;
	vec3_t		mins, maxs;

	if (node->contents == CONTENTS_SOLID)
		return;		// solid

	if (node->visframe != r_visframecount)
		return;
	if (R_CullBox (node->minmaxs, node->minmaxs+3))
		return;
	
// if a leaf node, draw stuff
	if (node->contents < 0)
	{
		pleaf = (mleaf_t *)node;

		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		if (c)
		{
			do
			{
				(*mark)->visframe = r_framecount;
				mark++;
			} while (--c);
		}

	// deal with model fragments in this leaf
		if (pleaf->efrags)
			R_StoreEfrags (&pleaf->efrags);

		return;
	}

// node is just a decision point, so go down the apropriate sides

// find which side of the node we are on
	plane = node->plane;

	switch (plane->type)
	{
	case PLANE_X:
		dot = modelorg[0] - plane->dist;
		break;
	case PLANE_Y:
		dot = modelorg[1] - plane->dist;
		break;
	case PLANE_Z:
		dot = modelorg[2] - plane->dist;
		break;
	default:
		dot = DotProduct (modelorg, plane->normal) - plane->dist;
		break;
	}

	if (dot >= 0)
		side = 0;
	else
		side = 1;

// recurse down the children, front side first
	R_RecursiveWorldNode (node->children[side]);

// draw stuff
	c = node->numsurfaces;

	if (c)
	{
		surf = cl.worldmodel->surfaces + node->firstsurface;

		if (dot < 0 -BACKFACE_EPSILON)
			side = SURF_PLANEBACK;
		else if (dot > BACKFACE_EPSILON)
			side = 0;
		{
			for ( ; c ; c--, surf++)
			{
				if (surf->visframe != r_framecount)
					continue;

				// don't backface underwater surfaces, because they warp
				if ( !(surf->flags & SURF_UNDERWATER) && ( (dot < 0) ^ !!(surf->flags & SURF_PLANEBACK)) )
					continue;		// wrong side

				// if sorting by texture, just store it out
				if (1)
				{
					if (!mirror
					|| surf->texinfo->texture != cl.worldmodel->textures[mirrortexturenum])
					{
						surf->texturechain = surf->texinfo->texture->texturechain;
						surf->texinfo->texture->texturechain = surf;
					}
				}
			}
		}

	}

// recurse down the back side
	R_RecursiveWorldNode (node->children[!side]);
}



/*
=============
R_DrawWorld
=============
*/
void R_DrawWorld (void)
{
	entity_t	ent;
	int			i;

	memset (&ent, 0, sizeof(ent));
	ent.model = cl.worldmodel;

	VectorCopy (r_refdef.vieworg, modelorg);

	currententity = &ent;
	currenttexture = -1;

	memset (lightmap_polys, 0, sizeof(lightmap_polys));

	R_RecursiveWorldNode (cl.worldmodel->nodes);
	StartBrushBatch(gldepthmin, gldepthmax);
	DrawTextureChains(0);

	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(0.75f, 0.75f);
	for (i = 0; i<cl_numvisedicts; i++)
	{
		currententity = cl_visedicts[i];
		switch (currententity->model->type)
		{
		case mod_brush:
			R_DrawBrushModel(currententity);
			break;
		default:
			break;
		}
	}
	UpdateTransformUBOs();
	glPolygonOffset(0, 0);
	glDisable(GL_POLYGON_OFFSET_FILL);

	SetupWarpBatch();
	DrawTextureChains(1);
	SetupSkyBatch();
	DrawTextureChains(2);
	EndBrushBatch();
}


/*
===============
R_MarkLeaves
===============
*/
void R_MarkLeaves (void)
{
	byte	*vis;
	mnode_t	*node;
	int		i;
	byte	solid[4096];

	if (r_oldviewleaf == r_viewleaf && !r_novis.value)
		return;
	
	if (mirror)
		return;

	r_visframecount++;
	r_oldviewleaf = r_viewleaf;

	if (r_novis.value)
	{
		vis = solid;
		memset (solid, 0xff, (cl.worldmodel->numleafs+7)>>3);
	}
	else
		vis = Mod_LeafPVS (r_viewleaf, cl.worldmodel);
		
	for (i=0 ; i<cl.worldmodel->numleafs ; i++)
	{
		if (vis[i>>3] & (1<<(i&7)))
		{
			node = (mnode_t *)&cl.worldmodel->leafs[i+1];
			do
			{
				if (node->visframe == r_visframecount)
					break;
				node->visframe = r_visframecount;
				node = node->parent;
			} while (node);
		}
	}
}



/*
=============================================================================

  LIGHTMAP ALLOCATION

=============================================================================
*/

// returns a texture number and the position inside it
int AllocBlock (int w, int h, int *x, int *y)
{
	int		i, j;
	int		best, best2;
	int		bestx;
	int		texnum;

	for (texnum=0 ; texnum<MAX_LIGHTMAPS ; texnum++)
	{
		best = BLOCK_HEIGHT;

		for (i=0 ; i<BLOCK_WIDTH-w ; i++)
		{
			best2 = 0;

			for (j=0 ; j<w ; j++)
			{
				if (allocated[texnum][i+j] >= best)
					break;
				if (allocated[texnum][i+j] > best2)
					best2 = allocated[texnum][i+j];
			}
			if (j == w)
			{	// this is a valid spot
				*x = i;
				*y = best = best2;
			}
		}

		if (best + h > BLOCK_HEIGHT)
			continue;

		for (i=0 ; i<w ; i++)
			allocated[texnum][*x + i] = best + h;

		return texnum;
	}

	Sys_Error ("AllocBlock: full");
	return 0;
}


mvertex_t	*r_pcurrentvertbase;
model_t		*currentmodel;

int	nColinElim;

/*
================
BuildSurfaceDisplayList
================
*/
void BuildSurfaceDisplayList (msurface_t *fa)
{
	int			i, lindex, lnumverts, s_axis, t_axis;
	float		dist, lastdist, lzi, scale, u, v, frac;
	unsigned	mask;
	vec3_t		local, transformed;
	medge_t		*pedges, *r_pedge;
	mplane_t	*pplane;
	int			vertpage, newverts, newpage, lastvert;
	qboolean	visible;
	float		*vec;
	float		s, t;
	glpoly_t	*poly;

// reconstruct the polygon
	pedges = currentmodel->edges;
	lnumverts = fa->numedges;
	vertpage = 0;

	//
	// draw texture
	//
	poly = Hunk_Alloc (sizeof(glpoly_t) + (lnumverts-4) * VERTEXSIZE*sizeof(float));
	poly->next = fa->polys;
	poly->flags = fa->flags;
	fa->polys = poly;
	poly->numverts = lnumverts;

	for (i=0 ; i<lnumverts ; i++)
	{
		lindex = currentmodel->surfedges[fa->firstedge + i];

		if (lindex > 0)
		{
			r_pedge = &pedges[lindex];
			vec = r_pcurrentvertbase[r_pedge->v[0]].position;
		}
		else
		{
			r_pedge = &pedges[-lindex];
			vec = r_pcurrentvertbase[r_pedge->v[1]].position;
		}
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s /= fa->texinfo->texture->width;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t /= fa->texinfo->texture->height;

		VectorCopy (vec, poly->verts[i]);
		poly->verts[i][3] = s;
		poly->verts[i][4] = t;

		//
		// lightmap texture coordinates
		//
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s -= fa->texturemins[0];
		s += fa->light_s*16;
		s += 8;
		s /= BLOCK_WIDTH * 16; //fa->texinfo->texture->width;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t -= fa->texturemins[1];
		t += fa->light_t*16;
		t += 8;
		t /= BLOCK_HEIGHT * 16; //fa->texinfo->texture->height;

		// s and t are now in the range of BLOCK_HEIGHT
		s /= ATLAS_WIDTH;
		t /= ATLAS_HEIGHT;
		// 
		s += ((float)(fa->sOffset*BLOCK_WIDTH) / (float)(ATLAS_WIDTH * BLOCK_WIDTH));
		t += ((float)(fa->tOffset*BLOCK_HEIGHT) / (float)(ATLAS_HEIGHT * BLOCK_HEIGHT));

		if (fa->flags & SURF_DRAWTURB || fa->flags & SURF_DRAWSKY)
		{
			s = -1.0f;
			t = -1.0f;
		}
		poly->verts[i][5] = s;
		poly->verts[i][6] = t;
	}

	//
	// remove co-linear points - Ed
	//
	if (!gl_keeptjunctions.value && !(fa->flags & SURF_UNDERWATER) )
	{
		for (i = 0 ; i < lnumverts ; ++i)
		{
			vec3_t v1, v2;
			float *prev, *this, *next;
			float f;

			prev = poly->verts[(i + lnumverts - 1) % lnumverts];
			this = poly->verts[i];
			next = poly->verts[(i + 1) % lnumverts];

			VectorSubtract( this, prev, v1 );
			VectorNormalize( v1 );
			VectorSubtract( next, prev, v2 );
			VectorNormalize( v2 );

			// skip co-linear points
			#define COLINEAR_EPSILON 0.001
			if ((fabs( v1[0] - v2[0] ) <= COLINEAR_EPSILON) &&
				(fabs( v1[1] - v2[1] ) <= COLINEAR_EPSILON) && 
				(fabs( v1[2] - v2[2] ) <= COLINEAR_EPSILON))
			{
				int j;
				for (j = i + 1; j < lnumverts; ++j)
				{
					int k;
					for (k = 0; k < VERTEXSIZE; ++k)
						poly->verts[j - 1][k] = poly->verts[j][k];
				}
				--lnumverts;
				++nColinElim;
				// retry next vertex next time, which is now current vertex
				--i;
			}
		}
	}
	poly->numverts = lnumverts;

}

/*
========================
GL_CreateSurfaceLightmap
========================
*/
void GL_CreateSurfaceLightmap (msurface_t *surf)
{
	int		smax, tmax, s, t, l, i, counter;
	byte	*base;

	if (surf->flags & (SURF_DRAWSKY|SURF_DRAWTURB))
		return;

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;

	surf->lightmaptexturenum = AllocBlock (smax, tmax, &surf->light_s, &surf->light_t);
	surf->sOffset = surf->lightmaptexturenum % ATLAS_WIDTH;
	surf->tOffset = 0;
	counter = surf->lightmaptexturenum;
	while (counter >= ATLAS_WIDTH)
	{
		counter -= ATLAS_WIDTH;
		surf->tOffset++;
	}
	//might need to see if we can just update the whole lightmap at once 
	//base = lightmapsData + lightmapsDataOffets[surf->lightmaptexturenum];
	//base += ((surf->light_t * BLOCK_WIDTH*ATLAS_WIDTH) + surf->light_s) * lightmap_bytes;
	//R_BuildLightMap (surf, base, ATLAS_WIDTH* BLOCK_WIDTH*lightmap_bytes);
	base = lightmapsData + surf->lightmaptexturenum*lightmap_bytes*BLOCK_WIDTH*BLOCK_HEIGHT;
	base += (surf->light_t * BLOCK_WIDTH + surf->light_s) * lightmap_bytes;
	R_BuildLightMap (surf, base, BLOCK_WIDTH*lightmap_bytes);
}


/*
==================
GL_BuildLightmaps

Builds the lightmap texture
with all the surfaces from all brush models
==================
*/

void GL_DestroyLightmaps (void)
{
	glDeleteTextures(4, lightmap_textures_gl);
}

void GL_BuildLightmaps (void)
{
	int		i, j;
	model_t	*m;
	extern qboolean isPermedia;
	int numVertices = 0;
	int vertexOffset = 0;

	memset (allocated, 0, sizeof(allocated));

	r_framecount = 1;		// no dlightcache

	static int lightmap_init = false;
	if (!lightmap_init)
	{
		glGenTextures(4, lightmap_textures_gl);
		lightmap_init = true;
	}

#ifdef _WIN32
	gl_lightmap_format = GL_RED;
	// default differently on the Permedia
	if (isPermedia)
		gl_lightmap_format = GL_RGBA;
	
	if (COM_CheckParm ("-lm_1"))
		gl_lightmap_format = GL_RED;
	if (COM_CheckParm ("-lm_a"))
		gl_lightmap_format = GL_RED;
	if (COM_CheckParm ("-lm_i"))
		gl_lightmap_format = GL_RED;
	if (COM_CheckParm ("-lm_2"))
		gl_lightmap_format = GL_RG;
	if (COM_CheckParm ("-lm_4"))
		gl_lightmap_format = GL_RGBA;
#else
	gl_lightmap_format = GL_ALPHA;
	// default differently on the Permedia
	if (isPermedia)
		gl_lightmap_format = GL_RGBA;

	if (COM_CheckParm ("-lm_1"))
		gl_lightmap_format = GL_ALPHA;
	if (COM_CheckParm ("-lm_a"))
		gl_lightmap_format = GL_ALPHA;
	if (COM_CheckParm ("-lm_i"))
		gl_lightmap_format = GL_ALPHA;
	if (COM_CheckParm ("-lm_2"))
		gl_lightmap_format = GL_LUMINANCE_ALPHA;
	if (COM_CheckParm ("-lm_4"))
		gl_lightmap_format = GL_RGBA;
#endif
	switch (gl_lightmap_format)
	{
	case GL_RGBA:
	//	lightmap_bytes = 4;
	//	break;
	case GL_RG:
	//case GL_LUMINANCE_ALPHA:
	//	lightmap_bytes = 2;
	//	break;
	default:
	//case GL_ALPHA:	
	case GL_RED:	
		lightmap_bytes = 1;
		break;
	}

	for (i = 0; i < MAX_LIGHTMAPS; i++)
	{
		int sOffset = i % ATLAS_WIDTH;
		int tOffset = 0;
		int counter = i;
		while (counter >= ATLAS_WIDTH)
		{
			counter -= ATLAS_WIDTH;
			tOffset++;
		}
		lightmapsDataOffets[i] = tOffset*lightmap_bytes*BLOCK_HEIGHT*BLOCK_WIDTH*ATLAS_WIDTH;	//height into the atlas
		lightmapsDataOffets[i] += sOffset * lightmap_bytes*BLOCK_WIDTH;							//block width into the atlas
		lightmapsGLSOffets[i] = sOffset*BLOCK_WIDTH;
		lightmapsGLTOffets[i] = tOffset*BLOCK_HEIGHT;
	}

	for (j=1 ; j<MAX_MODELS ; j++)
	{
		m = cl.model_precache[j];
		if (!m)
			break;
		if (m->name[0] == '*')
			continue;
		r_pcurrentvertbase = m->vertexes;
		currentmodel = m;
		for (i=0 ; i<m->numsurfaces ; i++)
		{
			GL_CreateSurfaceLightmap (m->surfaces + i);
			#if SUBDIVIDE_WARP_POLYS
			if ( m->surfaces[i].flags & SURF_DRAWSKY)
			{
				glpoly_t	*p;
				//BuildSurfaceDisplayList (m->surfaces + i);
				for (p=m->surfaces[i].polys ; p ; p=p->next)
				{
					numVertices += p->numverts;
				}
			}
			else
			#endif
			{
				BuildSurfaceDisplayList (m->surfaces + i);
				numVertices += m->surfaces[i].polys->numverts;
			}
		}
	}
	if(numVertices > 65535)
	{
		printf("we have goofed, need to break up meshes for raspberry pi");
	}


	// allocate buffer
	CreateBrushBuffers(numVertices);
	vertexOffset = 0;

	//dump the triangle data to a buffer
	glBrushData * pBrushData = (glBrushData*)malloc( sizeof(glBrushData) * numVertices );
	int offset = 0;
	
	for (j=1 ; j<MAX_MODELS ; j++)
	{
		m = cl.model_precache[j];
		if (!m)
			break;
		if (m->name[0] == '*')
			continue;

		//for each model allocate a vbo and set offsets for each polygon
		for (i=0 ; i<m->numsurfaces ; i++)
		{
			int t = 0;
			float	*v = m->surfaces[i].polys->verts[0];	
			#if SUBDIVIDE_WARP_POLYS
			if ( m->surfaces[i].flags & SURF_DRAWSKY)
			{
				glpoly_t	*p;				
				for (p=m->surfaces[i].polys ; p ; p=p->next)
				{
					v = p->verts[0];	
					for (t=0 ; t<p->numverts ; t++, v+= VERTEXSIZE)
					{
						pBrushData[offset].pos[0] = v[0];	//position
						pBrushData[offset].pos[1] = v[1];	//position
						pBrushData[offset].pos[2] = v[2];	//position
			
						pBrushData[offset].st1[0] = v[3]; pBrushData[offset].st1[1] = v[4];	//textures
						pBrushData[offset].st2[0] = v[5]; pBrushData[offset].st2[1] = v[6];	//textures
			
						offset++;
					}
					p->vertexOffset = vertexOffset;
					vertexOffset += p->numverts;
				}
			}
			else
			#endif
			{
				for (t=0 ; t<m->surfaces[i].polys->numverts ; t++, v+= VERTEXSIZE)
				{
					pBrushData[offset].pos[0] = v[0];	//position
					pBrushData[offset].pos[1] = v[1];	//position
					pBrushData[offset].pos[2] = v[2];	//position

					pBrushData[offset].st1[0] = v[3]; pBrushData[offset].st1[1] = v[4];	//textures Colour
					pBrushData[offset].st2[0] = v[5]; pBrushData[offset].st2[1] = v[6];	//textures Lightmaps

					offset++;
				}
				m->surfaces[i].polys->vertexOffset = vertexOffset;
				vertexOffset += m->surfaces[i].polys->numverts;
			}
		}
	}

	// upload data
	AddBrushData( 0, vertexOffset, pBrushData );
	//free temp buffer
	free(pBrushData);

	//
	// upload all lightmaps that were filled
	//

	lightmap_active_index = 0;
	for (i = 0; i < 4; i++)
	{
		//float borderColour[] = { 1.0f, 1.0f, 1.0f, 1.0f };
		GL_BindNoFlush(lightmap_textures_gl[i], TEX_SLOT_LIGHT_1);
		glTexImage2D(GL_TEXTURE_2D, 0, texDataType[lightmap_bytes],
					 (BLOCK_WIDTH * ATLAS_WIDTH), (BLOCK_HEIGHT * ATLAS_HEIGHT),
					 0, texDataType[lightmap_bytes], GL_UNSIGNED_BYTE, 0);
		//lightmaps should never be nearest i think
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}

	for (i = 0; i<MAX_LIGHTMAPS; i++)
	{
		if (!allocated[i][0])
			break;		// no more used

		lightmap_modified[i] = false;
		lightmap_was_modified[i] = 0;
		lightmap_rectchange[i].l = lightmap_rectchange_cache[i].l = BLOCK_WIDTH;
		lightmap_rectchange[i].t = lightmap_rectchange_cache[i].t = BLOCK_HEIGHT;
		lightmap_rectchange[i].w = lightmap_rectchange_cache[i].w = 0;
		lightmap_rectchange[i].h = lightmap_rectchange_cache[i].h = 0;

		GL_BindNoFlush(lightmap_textures_gl[0], TEX_SLOT_LIGHT_0);
		glTexSubImage2D(GL_TEXTURE_2D, 0, lightmapsGLSOffets[i], lightmapsGLTOffets[i],
						BLOCK_WIDTH, BLOCK_HEIGHT, gl_lightmap_format, GL_UNSIGNED_BYTE,
						lightmapsData + (i* BLOCK_HEIGHT) *BLOCK_WIDTH*lightmap_bytes);
		GL_BindNoFlush(lightmap_textures_gl[1], TEX_SLOT_LIGHT_1);
		glTexSubImage2D(GL_TEXTURE_2D, 0, lightmapsGLSOffets[i], lightmapsGLTOffets[i],
						BLOCK_WIDTH, BLOCK_HEIGHT, gl_lightmap_format, GL_UNSIGNED_BYTE,
						lightmapsData + (i* BLOCK_HEIGHT) *BLOCK_WIDTH*lightmap_bytes);
		GL_BindNoFlush(lightmap_textures_gl[2], TEX_SLOT_LIGHT_2);
		glTexSubImage2D(GL_TEXTURE_2D, 0, lightmapsGLSOffets[i], lightmapsGLTOffets[i],
						BLOCK_WIDTH, BLOCK_HEIGHT, gl_lightmap_format, GL_UNSIGNED_BYTE,
						lightmapsData + (i* BLOCK_HEIGHT) *BLOCK_WIDTH*lightmap_bytes);
		GL_BindNoFlush(lightmap_textures_gl[3], TEX_SLOT_LIGHT_3);
		glTexSubImage2D(GL_TEXTURE_2D, 0, lightmapsGLSOffets[i], lightmapsGLTOffets[i],
						BLOCK_WIDTH, BLOCK_HEIGHT, gl_lightmap_format, GL_UNSIGNED_BYTE,
						lightmapsData + (i* BLOCK_HEIGHT) *BLOCK_WIDTH*lightmap_bytes);
	}
	GL_BindNoFlush(lightmap_textures_gl[0], TEX_SLOT_LIGHT_0);
	GL_BindNoFlush(lightmap_textures_gl[1], TEX_SLOT_LIGHT_1);
	GL_BindNoFlush(lightmap_textures_gl[2], TEX_SLOT_LIGHT_2);
	GL_BindNoFlush(lightmap_textures_gl[3], TEX_SLOT_LIGHT_3);
}

