/*
Copyright 2007 nVidia, Inc.
Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance with the License. 

You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0 

Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on an "AS IS" BASIS, 
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 

See the License for the specific language governing permissions and limitations under the License.
*/

// Thanks to Jacob Munkberg (jacob@cs.lth.se) for the shortcut of using SVD to do the equivalent of principal components analysis

//  x1		444.1x6 16p 45b (3bi)

#include "bits.h"
#include "tile.h"
#include "avpcl.h"
#include "arvo/Vec4.h"
#include "arvo/Matrix.h"
#include "arvo/SVD.h"
#include "utils.h"
#include "endpts.h"

#include <assert.h>

#include "shapes_three.h"

// use only the first 16 available shapes
#undef NSHAPES
#undef SHAPEBITS
#define NSHAPES 16
#define SHAPEBITS 4

using namespace ArvoMath;

#define	NLSBMODES	4		// number of different lsb modes per region. since we have two .1 per region, that can have 4 values

#define NINDICES	8
#define	INDEXBITS	3
#define	HIGH_INDEXBIT	(1<<(INDEXBITS-1))
#define	DENOM		(NINDICES-1)
#define	BIAS		(DENOM/2)

// WORK: determine optimal traversal pattern to search for best shape -- what does the error curve look like?
// i.e. can we search shapes in a particular order so we can see the global error minima easily and
// stop without having to touch all shapes?

#define	POS_TO_X(pos)	((pos)&3)
#define	POS_TO_Y(pos)	(((pos)>>2)&3)

#define	NBITSIZES	(NREGIONS*2)
#define	ABITINDEX(region)	(2*(region)+0)
#define	BBITINDEX(region)	(2*(region)+1)

struct ChanBits
{
	int nbitsizes[NBITSIZES];	// bitsizes for one channel
};

struct Pattern
{
	ChanBits chan[NCHANNELS_RGB];//  bit patterns used per channel
	int transformed;		// if 0, deltas are unsigned and no transform; otherwise, signed and transformed
	int mode;				// associated mode value
	int modebits;			// number of mode bits
	char *encoding;			// verilog description of encoding for this mode
};

#define	NPATTERNS 1

static Pattern patterns[NPATTERNS] =
{
	// red			green			blue			xfm	mode  mb
	4,4,4,4,4,4,	4,4,4,4,4,4,	4,4,4,4,4,4,	0,	0x1, 1, "",	// really 444.1 x 6
};

struct RegionPrec
{
	int	endpt_a_prec[NCHANNELS_RGB];
	int endpt_b_prec[NCHANNELS_RGB];
};

struct PatternPrec
{
	RegionPrec region_precs[NREGIONS];
};

// this is the precision for each channel and region
// NOTE: this MUST match the corresponding data in "patterns" above -- WARNING: there is NO assert to check this!
static PatternPrec pattern_precs[NPATTERNS] =
{
	4,4,4, 4,4,4, 4,4,4, 4,4,4, 4,4,4, 4,4,4, 
};

// return # of bits needed to store n. handle signed or unsigned cases properly
static int nbits(int n, bool issigned)
{
	int nb;
	if (n==0)
		return 0;	// no bits needed for 0, signed or not
	else if (n > 0)
	{
		for (nb=0; n; ++nb, n>>=1) ;
		return nb + (issigned?1:0);
	}
	else
	{
		assert (issigned);
		for (nb=0; n<-1; ++nb, n>>=1) ;
		return nb + 1;
	}
}

static void transform_forward(IntEndptsRGB_2 ep[NREGIONS])
{
	assert(0);
}

static void transform_inverse(IntEndptsRGB_2 ep[NREGIONS])
{
	assert(0);
}

// endpoints are 555,555; reduce to 444,444 and put the lsb bit majority in compr_bits
static void compress_one(const IntEndptsRGB& endpts, IntEndptsRGB_2& compr_endpts)
{
	int onescnt;

	onescnt = 0;
	for (int j=0; j<NCHANNELS_RGB; ++j)
	{
		onescnt += endpts.A[j] & 1;
		compr_endpts.A[j] = endpts.A[j] >> 1;
		assert (compr_endpts.A[j] < 16);
	}
	compr_endpts.a_lsb = onescnt >= 2;

	onescnt = 0;
	for (int j=0; j<NCHANNELS_RGB; ++j)
	{
		onescnt += endpts.B[j] & 1;
		compr_endpts.B[j] = endpts.B[j] >> 1;
		assert (compr_endpts.B[j] < 16);
	}
	compr_endpts.b_lsb = onescnt >= 2;
}

static void uncompress_one(const IntEndptsRGB_2& compr_endpts, IntEndptsRGB& endpts)
{
	for (int j=0; j<NCHANNELS_RGB; ++j)
	{
		endpts.A[j] = (compr_endpts.A[j] << 1) | compr_endpts.a_lsb;
		endpts.B[j] = (compr_endpts.B[j] << 1) | compr_endpts.b_lsb;
	}
}

static void uncompress_endpoints(const IntEndptsRGB_2 compr_endpts[NREGIONS], IntEndptsRGB endpts[NREGIONS])
{
	for (int i=0; i<NREGIONS; ++i)
		uncompress_one(compr_endpts[i], endpts[i]);
}

static void compress_endpoints(const IntEndptsRGB endpts[NREGIONS], IntEndptsRGB_2 compr_endpts[NREGIONS])
{
	for (int i=0; i<NREGIONS; ++i)
		compress_one(endpts[i], compr_endpts[i]);
}


static void quantize_endpts(const FltEndpts endpts[NREGIONS], const PatternPrec &pattern_prec, IntEndptsRGB_2 q_endpts[NREGIONS])
{
	IntEndptsRGB full_endpts[NREGIONS];

	for (int region = 0; region < NREGIONS; ++region)
	{
		full_endpts[region].A[0] = Utils::quantize(endpts[region].A.X(), pattern_prec.region_precs[region].endpt_a_prec[0]+1);	// +1 since we are in uncompressed space
		full_endpts[region].A[1] = Utils::quantize(endpts[region].A.Y(), pattern_prec.region_precs[region].endpt_a_prec[1]+1);
		full_endpts[region].A[2] = Utils::quantize(endpts[region].A.Z(), pattern_prec.region_precs[region].endpt_a_prec[2]+1);
		full_endpts[region].B[0] = Utils::quantize(endpts[region].B.X(), pattern_prec.region_precs[region].endpt_b_prec[0]+1);
		full_endpts[region].B[1] = Utils::quantize(endpts[region].B.Y(), pattern_prec.region_precs[region].endpt_b_prec[1]+1);
		full_endpts[region].B[2] = Utils::quantize(endpts[region].B.Z(), pattern_prec.region_precs[region].endpt_b_prec[2]+1);
		compress_one(full_endpts[region], q_endpts[region]);
	}
}

// swap endpoints as needed to ensure that the indices at index_positions have a 0 high-order bit
static void swap_indices(IntEndptsRGB_2 endpts[NREGIONS], int indices[Tile::TILE_H][Tile::TILE_W], int shapeindex)
{
	for (int region = 0; region < NREGIONS; ++region)
	{
		int position = SHAPEINDEX_TO_COMPRESSED_INDICES(shapeindex,region);

		int x = POS_TO_X(position);
		int y = POS_TO_Y(position);
		assert(REGION(x,y,shapeindex) == region);		// double check the table
		if (indices[y][x] & HIGH_INDEXBIT)
		{
			// high bit is set, swap the endpts and indices for this region
			int t;
			for (int i=0; i<NCHANNELS_RGB; ++i) 
			{
				t = endpts[region].A[i]; endpts[region].A[i] = endpts[region].B[i]; endpts[region].B[i] = t;
			}
			t = endpts[region].a_lsb; endpts[region].a_lsb = endpts[region].b_lsb; endpts[region].b_lsb = t;

			for (int y = 0; y < Tile::TILE_H; y++)
			for (int x = 0; x < Tile::TILE_W; x++)
				if (REGION(x,y,shapeindex) == region)
					indices[y][x] = NINDICES - 1 - indices[y][x];
		}
	}
}

static bool endpts_fit(IntEndptsRGB_2 endpts[NREGIONS], const Pattern &p)
{
	return true;
}

static void write_header(const IntEndptsRGB_2 endpts[NREGIONS], int shapeindex, const Pattern &p, Bits &out)
{
	out.write(p.mode, p.modebits);
	out.write(shapeindex, SHAPEBITS);

	for (int j=0; j<NCHANNELS_RGB; ++j)
		for (int i=0; i<NREGIONS; ++i)
		{
			out.write(endpts[i].A[j], p.chan[j].nbitsizes[ABITINDEX(i)]);
			out.write(endpts[i].B[j], p.chan[j].nbitsizes[BBITINDEX(i)]);
		}

	for (int i=0; i<NREGIONS; ++i)
	{
		out.write(endpts[i].a_lsb, 1);
		out.write(endpts[i].b_lsb, 1);
	}

	assert (out.getptr() == 83);
}

static void read_header(Bits &in, IntEndptsRGB_2 endpts[NREGIONS], int &shapeindex, Pattern &p, int &pat_index)
{
	int mode = AVPCL::getmode(in);

	pat_index = 0;
	assert (pat_index >= 0 && pat_index < NPATTERNS);
	assert (in.getptr() == patterns[pat_index].modebits);

	shapeindex = in.read(SHAPEBITS);
	p = patterns[pat_index];

	for (int j=0; j<NCHANNELS_RGB; ++j)
		for (int i=0; i<NREGIONS; ++i)
		{
			endpts[i].A[j] = in.read(p.chan[j].nbitsizes[ABITINDEX(i)]);
			endpts[i].B[j] = in.read(p.chan[j].nbitsizes[BBITINDEX(i)]);
		}
	
	for (int i=0; i<NREGIONS; ++i)
	{
		endpts[i].a_lsb  = in.read(1);
		endpts[i].b_lsb  = in.read(1);
	}

	assert (in.getptr() == 83);
}

static void write_indices(const int indices[Tile::TILE_H][Tile::TILE_W], int shapeindex, Bits &out)
{
	int positions[NREGIONS];

	for (int r = 0; r < NREGIONS; ++r)
		positions[r] = SHAPEINDEX_TO_COMPRESSED_INDICES(shapeindex,r);

	for (int pos = 0; pos < Tile::TILE_TOTAL; ++pos)
	{
		int x = POS_TO_X(pos);
		int y = POS_TO_Y(pos);

		bool match = false;

		for (int r = 0; r < NREGIONS; ++r)
			if (positions[r] == pos) { match = true; break; }

		out.write(indices[y][x], INDEXBITS - (match ? 1 : 0));
	}
}

static void read_indices(Bits &in, int shapeindex, int indices[Tile::TILE_H][Tile::TILE_W])
{
	int positions[NREGIONS];

	for (int r = 0; r < NREGIONS; ++r)
		positions[r] = SHAPEINDEX_TO_COMPRESSED_INDICES(shapeindex,r);

	for (int pos = 0; pos < Tile::TILE_TOTAL; ++pos)
	{
		int x = POS_TO_X(pos);
		int y = POS_TO_Y(pos);

		bool match = false;

		for (int r = 0; r < NREGIONS; ++r)
			if (positions[r] == pos) { match = true; break; }

		indices[y][x]= in.read(INDEXBITS - (match ? 1 : 0));
	}
}

static void emit_block(const IntEndptsRGB_2 endpts[NREGIONS], int shapeindex, const Pattern &p, const int indices[Tile::TILE_H][Tile::TILE_W], char *block)
{
	Bits out(block, AVPCL::BITSIZE);

	write_header(endpts, shapeindex, p, out);

	write_indices(indices, shapeindex, out);

	assert(out.getptr() == AVPCL::BITSIZE);
}

static void generate_palette_quantized(const IntEndptsRGB_2 &endpts_2, const RegionPrec &region_prec, Vec4 palette[NINDICES])
{
	IntEndptsRGB endpts;

	uncompress_one(endpts_2, endpts);

	// scale endpoints
	int a, b;			// really need a IntVec4...

	a = Utils::unquantize(endpts.A[0], region_prec.endpt_a_prec[0]+1);	// +1 since we are in uncompressed space
	b = Utils::unquantize(endpts.B[0], region_prec.endpt_b_prec[0]+1);

	// interpolate
	for (int i = 0; i < NINDICES; ++i)
		palette[i].X() = PALETTE_LERP(a, b, i, BIAS, DENOM);

	a = Utils::unquantize(endpts.A[1], region_prec.endpt_a_prec[1]+1); 
	b = Utils::unquantize(endpts.B[1], region_prec.endpt_b_prec[1]+1);

	// interpolate
	for (int i = 0; i < NINDICES; ++i)
		palette[i].Y() = PALETTE_LERP(a, b, i, BIAS, DENOM);

	a = Utils::unquantize(endpts.A[2], region_prec.endpt_a_prec[2]+1); 
	b = Utils::unquantize(endpts.B[2], region_prec.endpt_b_prec[2]+1);

	// interpolate
	for (int i = 0; i < NINDICES; ++i)
		palette[i].Z() = PALETTE_LERP(a, b, i, BIAS, DENOM);

	// constant alpha
	for (int i = 0; i < NINDICES; ++i)
		palette[i].W() = RGBA_MAX;
}

static void sign_extend(Pattern &p, IntEndptsRGB_2 endpts[NREGIONS])
{
	assert(0);
}

void AVPCL::decompress_mode0(const char *block, Tile &t)
{
	Bits in(block, AVPCL::BITSIZE);

	Pattern p;
	IntEndptsRGB_2 endpts[NREGIONS];
	int shapeindex, pat_index;

	read_header(in, endpts, shapeindex, p, pat_index);
	
	if (p.transformed)
	{
		sign_extend(p, endpts);
		transform_inverse(endpts);
	}

	Vec4 palette[NREGIONS][NINDICES];
	for (int r = 0; r < NREGIONS; ++r)
		generate_palette_quantized(endpts[r], pattern_precs[pat_index].region_precs[r], &palette[r][0]);

	int indices[Tile::TILE_H][Tile::TILE_W];

	read_indices(in, shapeindex, indices);

	assert(in.getptr() == AVPCL::BITSIZE);

	// lookup
	for (int y = 0; y < Tile::TILE_H; y++)
	for (int x = 0; x < Tile::TILE_W; x++)
		t.data[y][x] = palette[REGION(x,y,shapeindex)][indices[y][x]];
}

// given a collection of colors and quantized endpoints, generate a palette, choose best entries, and return a single toterr
static double map_colors(const Vec4 colors[], int np, const IntEndptsRGB_2 &endpts, const RegionPrec &region_prec, double current_err, int indices[Tile::TILE_TOTAL])
{
	Vec4 palette[NINDICES];
	double toterr = 0;
	Vec4 err;

	generate_palette_quantized(endpts, region_prec, palette);

	for (int i = 0; i < np; ++i)
	{
		double err, besterr = DBL_MAX;

		for (int j = 0; j < NINDICES && besterr > 0; ++j)
		{
			err = Utils::metric4(colors[i], palette[j]);

			if (err > besterr)	// error increased, so we're done searching
				break;
			if (err < besterr)
			{
				besterr = err;
				indices[i] = j;
			}
		}
		toterr += besterr;

		// check for early exit
		if (toterr > current_err)
		{
			// fill out bogus index values so it's initialized at least
			for (int k = i; k < np; ++k)
				indices[k] = -1;

			return DBL_MAX;
		}
	}
	return toterr;
}

// assign indices given a tile, shape, and quantized endpoints, return toterr for each region
static void assign_indices(const Tile &tile, int shapeindex, IntEndptsRGB_2 endpts[NREGIONS], const PatternPrec &pattern_prec, 
						   int indices[Tile::TILE_H][Tile::TILE_W], double toterr[NREGIONS])
{
	// build list of possibles
	Vec4 palette[NREGIONS][NINDICES];

	for (int region = 0; region < NREGIONS; ++region)
	{
		generate_palette_quantized(endpts[region], pattern_prec.region_precs[region], &palette[region][0]);
		toterr[region] = 0;
	}

	Vec4 err;

	for (int y = 0; y < tile.size_y; y++)
	for (int x = 0; x < tile.size_x; x++)
	{
		int region = REGION(x,y,shapeindex);
		double err, besterr = DBL_MAX;

		for (int i = 0; i < NINDICES && besterr > 0; ++i)
		{
			err = Utils::metric4(tile.data[y][x], palette[region][i]);

			if (err > besterr)	// error increased, so we're done searching
				break;
			if (err < besterr)
			{
				besterr = err;
				indices[y][x] = i;
			}
		}
		toterr[region] += besterr;
	}
}

// note: indices are valid only if the value returned is less than old_err; otherwise they contain -1's
// this function returns either old_err or a value smaller (if it was successful in improving the error)
static double perturb_one(const Vec4 colors[], int np, int ch, const RegionPrec &region_prec, const IntEndptsRGB_2 &old_endpts, IntEndptsRGB_2 &new_endpts, 
						  double old_err, int do_b, int indices[Tile::TILE_TOTAL])
{
	// we have the old endpoints: old_endpts
	// we have the perturbed endpoints: new_endpts
	// we have the temporary endpoints: temp_endpts

	IntEndptsRGB_2 temp_endpts;
	float min_err = old_err;		// start with the best current error
	int beststep;
	int temp_indices[Tile::TILE_TOTAL];

	for (int i=0; i<np; ++i)
		indices[i] = -1;

	// copy real endpoints so we can perturb them
	temp_endpts = new_endpts = old_endpts;

	int prec = do_b ? region_prec.endpt_b_prec[ch] : region_prec.endpt_a_prec[ch];

	// do a logarithmic search for the best error for this endpoint (which)
	for (int step = 1 << (prec-1); step; step >>= 1)
	{
		bool improved = false;
		for (int sign = -1; sign <= 1; sign += 2)
		{
			if (do_b == 0)
			{
				temp_endpts.A[ch] = new_endpts.A[ch] + sign * step;
				if (temp_endpts.A[ch] < 0 || temp_endpts.A[ch] >= (1 << prec))
					continue;
			}
			else
			{
				temp_endpts.B[ch] = new_endpts.B[ch] + sign * step;
				if (temp_endpts.B[ch] < 0 || temp_endpts.B[ch] >= (1 << prec))
					continue;
			}

			float err = map_colors(colors, np, temp_endpts, region_prec, min_err, temp_indices);

			if (err < min_err)
			{
				improved = true;
				min_err = err;
				beststep = sign * step;
				for (int i=0; i<np; ++i)
					indices[i] = temp_indices[i];
			}
		}
		// if this was an improvement, move the endpoint and continue search from there
		if (improved)
		{
			if (do_b == 0)
				new_endpts.A[ch] += beststep;
			else
				new_endpts.B[ch] += beststep;
		}
	}
	return min_err;
}

// the larger the error the more time it is worth spending on an exhaustive search.
// perturb the endpoints at least -3 to 3.
// if err > 5000 perturb endpoints 50% of precision
// if err > 1000 25%
// if err > 200 12.5%
// if err > 40  6.25%
// for np = 16 -- adjust error thresholds as a function of np
// always ensure endpoint ordering is preserved (no need to overlap the scan)
// if orig_err returned from this is less than its input value, then indices[] will contain valid indices
static double exhaustive(const Vec4 colors[], int np, int ch, const RegionPrec &region_prec, double &orig_err, IntEndptsRGB_2 &opt_endpts, int indices[Tile::TILE_TOTAL])
{
	IntEndptsRGB_2 temp_endpts;
	double best_err = orig_err;
	int aprec = region_prec.endpt_a_prec[ch];
	int bprec = region_prec.endpt_b_prec[ch];
	int good_indices[Tile::TILE_TOTAL];
	int temp_indices[Tile::TILE_TOTAL];

	for (int i=0; i<np; ++i)
		indices[i] = -1;

	double thr_scale = (double)np / (double)Tile::TILE_TOTAL;

	if (orig_err == 0) return orig_err;

	int adelta = 0, bdelta = 0;
	if (orig_err > 5000.0*thr_scale)		{ adelta = (1 << aprec)/2; bdelta = (1 << bprec)/2; }
	else if (orig_err > 1000.0*thr_scale)	{ adelta = (1 << aprec)/4; bdelta = (1 << bprec)/4; }
	else if (orig_err > 200.0*thr_scale)	{ adelta = (1 << aprec)/8; bdelta = (1 << bprec)/8; }
	else if (orig_err > 40.0*thr_scale)		{ adelta = (1 << aprec)/16; bdelta = (1 << bprec)/16; }
	adelta = MAX(adelta, 3);
	bdelta = MAX(bdelta, 3);

#ifdef	DISABLE_EXHAUSTIVE
	adelta = bdelta = 3;
#endif

	temp_endpts = opt_endpts;

	// ok figure out the range of A and B
	int alow = MAX(0, opt_endpts.A[ch] - adelta);
	int ahigh = MIN((1<<aprec)-1, opt_endpts.A[ch] + adelta);
	int blow = MAX(0, opt_endpts.B[ch] - bdelta);
	int bhigh = MIN((1<<bprec)-1, opt_endpts.B[ch] + bdelta);

	// now there's no need to swap the ordering of A and B
	bool a_le_b = opt_endpts.A[ch] <= opt_endpts.B[ch];

	int amin, bmin;

	if (opt_endpts.A[ch] <= opt_endpts.B[ch])
	{
		// keep a <= b
		for (int a = alow; a <= ahigh; ++a)
		for (int b = MAX(a, blow); b < bhigh; ++b)
		{
			temp_endpts.A[ch] = a;
			temp_endpts.B[ch] = b;
		
			float err = map_colors(colors, np, temp_endpts, region_prec, best_err, temp_indices);
			if (err < best_err) 
			{ 
				amin = a; 
				bmin = b; 
				best_err = err;
				for (int i=0; i<np; ++i)
					good_indices[i] = temp_indices[i];
			}
		}
	}
	else
	{
		// keep b <= a
		for (int b = blow; b < bhigh; ++b)
		for (int a = MAX(b, alow); a <= ahigh; ++a)
		{
			temp_endpts.A[ch] = a;
			temp_endpts.B[ch] = b;
		
			float err = map_colors(colors, np, temp_endpts, region_prec, best_err, temp_indices);
			if (err < best_err) 
			{ 
				amin = a; 
				bmin = b; 
				best_err = err; 
				for (int i=0; i<np; ++i)
					good_indices[i] = temp_indices[i];
			}
		}
	}
	if (best_err < orig_err)
	{
		opt_endpts.A[ch] = amin;
		opt_endpts.B[ch] = bmin;
		orig_err = best_err;
		// if we actually improved, update the indices
		for (int i=0; i<np; ++i)
			indices[i] = good_indices[i];
	}
	return best_err;
}

static double optimize_one(const Vec4 colors[], int np, double orig_err, const IntEndptsRGB_2 &orig_endpts, const RegionPrec &region_prec, IntEndptsRGB_2 &opt_endpts)
{
	double opt_err = orig_err;

	opt_endpts = orig_endpts;

	/*
		err0 = perturb(rgb0, delta0)
		err1 = perturb(rgb1, delta1)
		if (err0 < err1)
			if (err0 >= initial_error) break
			rgb0 += delta0
			next = 1
		else
			if (err1 >= initial_error) break
			rgb1 += delta1
			next = 0
		initial_err = map()
		for (;;)
			err = perturb(next ? rgb1:rgb0, delta)
			if (err >= initial_err) break
			next? rgb1 : rgb0 += delta
			initial_err = err
	*/
	IntEndptsRGB_2 new_a, new_b;
	IntEndptsRGB_2 new_endpt;
	int do_b;
	int orig_indices[Tile::TILE_TOTAL];
	int new_indices[Tile::TILE_TOTAL];
	int temp_indices0[Tile::TILE_TOTAL];
	int temp_indices1[Tile::TILE_TOTAL];

	// now optimize each channel separately
	// for the first error improvement, we save the indices. then, for any later improvement, we compare the indices
	// if they differ, we restart the loop (which then falls back to looking for a first improvement.)
	for (int ch = 0; ch < NCHANNELS_RGB; ++ch)
	{
		// figure out which endpoint when perturbed gives the most improvement and start there
		// if we just alternate, we can easily end up in a local minima
		float err0 = perturb_one(colors, np, ch, region_prec, opt_endpts, new_a, opt_err, 0, temp_indices0);	// perturb endpt A
		float err1 = perturb_one(colors, np, ch, region_prec, opt_endpts, new_b, opt_err, 1, temp_indices1);	// perturb endpt B

		if (err0 < err1)
		{
			if (err0 >= opt_err)
				continue;

			for (int i=0; i<np; ++i)
			{
				new_indices[i] = orig_indices[i] = temp_indices0[i];
				assert (orig_indices[i] != -1);
			}

			opt_endpts.A[ch] = new_a.A[ch];
			opt_err = err0;
			do_b = 1;		// do B next
		}
		else
		{
			if (err1 >= opt_err)
				continue;

			for (int i=0; i<np; ++i)
			{
				new_indices[i] = orig_indices[i] = temp_indices1[i];
				assert (orig_indices[i] != -1);
			}

			opt_endpts.B[ch] = new_b.B[ch];
			opt_err = err1;
			do_b = 0;		// do A next
		}
		
		// now alternate endpoints and keep trying until there is no improvement
		for (;;)
		{
			float err = perturb_one(colors, np, ch, region_prec, opt_endpts, new_endpt, opt_err, do_b, temp_indices0);
			if (err >= opt_err)
				break;

			for (int i=0; i<np; ++i)
			{
				new_indices[i] = temp_indices0[i];
				assert (new_indices[i] != -1);
			}

			if (do_b == 0)
				opt_endpts.A[ch] = new_endpt.A[ch];
			else
				opt_endpts.B[ch] = new_endpt.B[ch];
			opt_err = err;
			do_b = 1 - do_b;	// now move the other endpoint
		}

		// see if the indices have changed
		int i;
		for (i=0; i<np; ++i)
			if (orig_indices[i] != new_indices[i])
				break;

		if (i<np)
			ch = -1;	// start over
	}

	// finally, do a small exhaustive search around what we think is the global minima to be sure
	// note this is independent of the above search, so we don't care about the indices from the above
	// we don't care about the above because if they differ, so what? we've already started at ch=0
	bool first = true;
	for (int ch = 0; ch < NCHANNELS_RGB; ++ch)
	{
		double new_err = exhaustive(colors, np, ch, region_prec, opt_err, opt_endpts, temp_indices0);

		if (new_err < opt_err)
		{
			opt_err = new_err;

			if (first)
			{
				for (int i=0; i<np; ++i)
				{
					orig_indices[i] = temp_indices0[i];
					assert (orig_indices[i] != -1);
				}
				first = false;
			}
			else
			{
				// see if the indices have changed
				int i;
				for (i=0; i<np; ++i)
					if (orig_indices[i] != temp_indices0[i])
						break;

				if (i<np)
				{
					ch = -1;	// start over
					first = true;
				}
			}
		}
	}

	return opt_err;
}

// this will return a valid set of endpoints in opt_endpts regardless of whether it improve orig_endpts or not
static void optimize_endpts(const Tile &tile, int shapeindex, const double orig_err[NREGIONS], 
							const IntEndptsRGB_2 orig_endpts[NREGIONS], const PatternPrec &pattern_prec, double opt_err[NREGIONS], IntEndptsRGB_2 opt_endpts[NREGIONS])
{
	Vec4 pixels[Tile::TILE_TOTAL];
	IntEndptsRGB_2 temp_in, temp_out;
	int temp_indices[Tile::TILE_TOTAL];

	for (int region=0; region<NREGIONS; ++region)
	{
		// collect the pixels in the region
		int np = 0;

		for (int y = 0; y < tile.size_y; y++)
		for (int x = 0; x < tile.size_x; x++)
			if (REGION(x,y,shapeindex) == region)
				pixels[np++] = tile.data[y][x];

		opt_endpts[region] = temp_in = orig_endpts[region];
		opt_err[region] = orig_err[region];

		double best_err = orig_err[region];

		for (int lsbmode=0; lsbmode<NLSBMODES; ++lsbmode)
		{
			temp_in.a_lsb = lsbmode & 1;
			temp_in.b_lsb = (lsbmode >> 1) & 1;

			// make sure we have a valid error for temp_in
			// we use DBL_MAX here because we want an accurate temp_in_err, no shortcuts
			// (mapcolors will compute a mapping but will stop if the error exceeds the value passed in the DBL_MAX position)
			double temp_in_err = map_colors(pixels, np, temp_in, pattern_prec.region_precs[region], DBL_MAX, temp_indices);

			// now try to optimize these endpoints
			double temp_out_err = optimize_one(pixels, np, temp_in_err, temp_in, pattern_prec.region_precs[region], temp_out);

			// if we find an improvement, update the best so far and correct the output endpoints and errors
			if (temp_out_err < best_err)
			{
				best_err = temp_out_err;
				opt_err[region] = temp_out_err;
				opt_endpts[region] = temp_out;
			}
		}
	}
}

/* optimization algorithm
	for each pattern
		convert endpoints using pattern precision
		assign indices and get initial error
		compress indices (and possibly reorder endpoints)
		transform endpoints
		if transformed endpoints fit pattern
			get original endpoints back
			optimize endpoints, get new endpoints, new indices, and new error // new error will almost always be better
			compress new indices
			transform new endpoints
			if new endpoints fit pattern AND if error is improved
				emit compressed block with new data
			else
				emit compressed block with original data // to try to preserve maximum endpoint precision
*/

static double refine(const Tile &tile, int shapeindex_best, const FltEndpts endpts[NREGIONS], char *block)
{
	double orig_err[NREGIONS], opt_err[NREGIONS], orig_toterr, opt_toterr, expected_opt_err[NREGIONS];
	IntEndptsRGB_2 orig_endpts[NREGIONS], opt_endpts[NREGIONS];
	int orig_indices[Tile::TILE_H][Tile::TILE_W], opt_indices[Tile::TILE_H][Tile::TILE_W];

	for (int sp = 0; sp < NPATTERNS; ++sp)
	{
		quantize_endpts(endpts, pattern_precs[sp], orig_endpts);
		assign_indices(tile, shapeindex_best, orig_endpts, pattern_precs[sp], orig_indices, orig_err);
		swap_indices(orig_endpts, orig_indices, shapeindex_best);
		if (patterns[sp].transformed)
			transform_forward(orig_endpts);
		// apply a heuristic here -- we check if the endpoints fit before we try to optimize them.
		// the assumption made is that if they don't fit now, they won't fit after optimizing.
		if (endpts_fit(orig_endpts, patterns[sp]))
		{
			if (patterns[sp].transformed)
				transform_inverse(orig_endpts);
			optimize_endpts(tile, shapeindex_best, orig_err, orig_endpts, pattern_precs[sp], expected_opt_err, opt_endpts);
			assign_indices(tile, shapeindex_best, opt_endpts, pattern_precs[sp], opt_indices, opt_err);
			for (int i=0; i<NREGIONS; ++i)
				assert(expected_opt_err[i] == opt_err[i]);
			swap_indices(opt_endpts, opt_indices, shapeindex_best);
			if (patterns[sp].transformed)
				transform_forward(opt_endpts);
			orig_toterr = opt_toterr = 0;
			for (int i=0; i < NREGIONS; ++i) { orig_toterr += orig_err[i]; opt_toterr += opt_err[i]; }
			if (endpts_fit(opt_endpts, patterns[sp]) && opt_toterr < orig_toterr)
			{
				emit_block(opt_endpts, shapeindex_best, patterns[sp], opt_indices, block);
				return opt_toterr;
			}
			else
			{
				// either it stopped fitting when we optimized it, or there was no improvement
				// so go back to the unoptimized endpoints which we know will fit
				if (patterns[sp].transformed)
					transform_forward(orig_endpts);
				emit_block(orig_endpts, shapeindex_best, patterns[sp], orig_indices, block);
				return orig_toterr;
			}
		}
	}
	throw "No candidate found, should never happen (mode avpcl 0).";
}

static void clamp(Vec4 &v)
{
	if (v.X() < RGBA_MIN) v.X() = RGBA_MIN;
	if (v.X() > RGBA_MAX) v.X() = RGBA_MAX;
	if (v.Y() < RGBA_MIN) v.Y() = RGBA_MIN;
	if (v.Y() > RGBA_MAX) v.Y() = RGBA_MAX;
	if (v.Z() < RGBA_MIN) v.Z() = RGBA_MIN;
	if (v.Z() > RGBA_MAX) v.Z() = RGBA_MAX;
	v.W() = RGBA_MAX;
}

static void generate_palette_unquantized(const FltEndpts endpts[NREGIONS], Vec4 palette[NREGIONS][NINDICES])
{
	for (int region = 0; region < NREGIONS; ++region)
	for (int i = 0; i < NINDICES; ++i)
		palette[region][i] = PALETTE_LERP(endpts[region].A, endpts[region].B, i, 0.0, float(DENOM));
}

// generate a palette from unquantized endpoints, then pick best palette color for all pixels in each region, return toterr for all regions combined
static double map_colors(const Tile &tile, int shapeindex, const FltEndpts endpts[NREGIONS])
{
	// build list of possibles
	Vec4 palette[NREGIONS][NINDICES];

	generate_palette_unquantized(endpts, palette);

	double toterr = 0;
	Vec4 err;

	for (int y = 0; y < tile.size_y; y++)
	for (int x = 0; x < tile.size_x; x++)
	{
		int region = REGION(x,y,shapeindex);
		double err, besterr = DBL_MAX;

		for (int i = 0; i < NINDICES && besterr > 0; ++i)
		{
			err = Utils::metric4(tile.data[y][x], palette[region][i]);

			if (err > besterr)	// error increased, so we're done searching. this works for most norms.
				break;
			if (err < besterr)
				besterr = err;
		}
		toterr += besterr;
	}
	return toterr;
}

// for this mode, we assume alpha = 255 constant and compress only the RGB portion.
// however, we do the error check against the actual alpha values supplied for the tile.
static double rough(const Tile &tile, int shapeindex, FltEndpts endpts[NREGIONS])
{
	for (int region=0; region<NREGIONS; ++region)
	{
		int np = 0;
		Vec4 colors[Tile::TILE_TOTAL];
		Vec4 mean(0,0,0,0);

		for (int y = 0; y < tile.size_y; y++)
		for (int x = 0; x < tile.size_x; x++)
			if (REGION(x,y,shapeindex) == region)
			{
				colors[np] = tile.data[y][x];
				mean += tile.data[y][x];
				++np;
			}

		// handle simple cases	
		if (np == 0)
		{
			Vec4 zero(0,0,0,RGBA_MAX);
			endpts[region].A = zero;
			endpts[region].B = zero;
			continue;
		}
		else if (np == 1)
		{
			endpts[region].A = colors[0];
			endpts[region].B = colors[0];
			continue;
		}
		else if (np == 2)
		{
			endpts[region].A = colors[0];
			endpts[region].B = colors[1];
			continue;
		}

		Matrix rdq(np, 3);

		mean /= float(np);

		// only look at RGB; ignore A
		for (int i = 0; i < np; ++i)
		{
			rdq(i,0) = colors[i].X() - mean.X();
			rdq(i,1) = colors[i].Y() - mean.Y();
			rdq(i,2) = colors[i].Z() - mean.Z();
		}
				
		// perform a singular value decomposition
		SVD svd(rdq);

		// get the principal component direction (the one with the largest weight)
		Vec4 direction(svd.R()(0,0), svd.R()(0,1), svd.R()(0,2), 0.0);

		// project each pixel value along the principal direction
		double minp = DBL_MAX, maxp = -DBL_MAX;
		for (int i = 0; i < np; i++) 
		{
			float dp = rdq(i,0) * direction.X() + rdq(i,1)*direction.Y() + rdq(i,2)*direction.Z();
			if (dp < minp) minp = dp;
			if (dp > maxp) maxp = dp;
		}

		// choose as endpoints 2 points along the principal direction that span the projections of all of the pixel values
		endpts[region].A = mean + minp*direction;
		endpts[region].B = mean + maxp*direction;

		// clamp endpoints
		// the argument for clamping is that the actual endpoints need to be clamped and thus we need to choose the best
		// shape based on endpoints being clamped
		clamp(endpts[region].A);
		clamp(endpts[region].B);
	}

	return map_colors(tile, shapeindex, endpts);
}

static void swap(double *list1, int *list2, int i, int j)
{
	double t = list1[i]; list1[i] = list1[j]; list1[j] = t;
	int t1 = list2[i]; list2[i] = list2[j]; list2[j] = t1;
}

double AVPCL::compress_mode0(const Tile &t, char *block)
{
	// number of rough cases to look at. reasonable values of this are 1, NSHAPES/4, and NSHAPES
	// NSHAPES/4 gets nearly all the cases; you can increase that a bit (say by 3 or 4) if you really want to squeeze the last bit out
	const int NITEMS=NSHAPES/4;

	// pick the best NITEMS shapes and refine these.
	struct {
		FltEndpts endpts[NREGIONS];
	} all[NSHAPES];
	double roughmse[NSHAPES];
	int index[NSHAPES];
	char tempblock[AVPCL::BLOCKSIZE];
	double msebest = DBL_MAX;

	for (int i=0; i<NSHAPES; ++i)
	{
		roughmse[i] = rough(t, i, &all[i].endpts[0]);
		index[i] = i;
	}

	// bubble sort -- only need to bubble up the first NITEMS items
	for (int i=0; i<NITEMS; ++i)
	for (int j=i+1; j<NSHAPES; ++j)
		if (roughmse[i] > roughmse[j])
			swap(roughmse, index, i, j);

	for (int i=0; i<NITEMS && msebest>0; ++i)
	{
		int shape = index[i];
		double mse = refine(t, shape, &all[shape].endpts[0], tempblock);
		if (mse < msebest)
		{
			memcpy(block, tempblock, sizeof(tempblock));
			msebest = mse;
		}
	}
	return msebest;
}

