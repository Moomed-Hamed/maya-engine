// voxel world : infinite streaming chunks shared by the playable world and the
// free-fly terrain lab. this header sits after player.h in the chain, so
// GameRenderer, Physics and the mesh helpers are already available.

/* ------------------------- *
	-         World         -
 * ------------------------- */

const int WORLD_CHUNK          = 16;
const int WORLD_HEIGHT         = 96;
const int WORLD_VIEW_RADIUS    = 16;                          // 33 x 33 loaded chunks
const int WORLD_VIEW_CHUNKS    = WORLD_VIEW_RADIUS * 2 + 1;
const int WORLD_MAX_CHUNKS     = WORLD_VIEW_CHUNKS * WORLD_VIEW_CHUNKS;

// streaming work is spread over frames so crossing a chunk border never stalls
// the frame. a full build (opening the scene) still happens in one go
const int WORLD_GEN_BUDGET     = 2;                           // new chunks per frame
const int WORLD_MESH_BUDGET    = 4;                           // dirty chunks per frame

enum World_Block : uint8
{
	WORLD_AIR = 0,
	WORLD_GRASS,
	WORLD_DIRT,
	WORLD_STONE,
	WORLD_SAND,
	WORLD_SNOW,
	WORLD_WOOD,
	WORLD_LEAVES,
	WORLD_PLANKS,
	WORLD_DRY_GRASS,
	WORLD_RED_SAND,
	WORLD_PINE_LEAVES,
	WORLD_JUNGLE_LEAVES,
	WORLD_TALL_GRASS,
	WORLD_FLOWER,
	WORLD_TORCH,
	WORLD_LIGHT,
	WORLD_BLOCK_COUNT
};

struct World_Block_Def
{
	const char* name;
	vec3  albedo;
	float roughness;
};

static const World_Block_Def WORLD_BLOCKS[WORLD_BLOCK_COUNT] = {
	{ "air",   vec3(0.f),                   1.f   },
	{ "grass", vec3(.30f, .50f, .20f),      .97f  },
	{ "dirt",  vec3(.42f, .30f, .18f),      .98f  },
	{ "stone", vec3(.40f, .42f, .45f),      .92f  },
	{ "sand",  vec3(.76f, .69f, .46f),      .97f  },
	{ "snow",  vec3(.88f, .91f, .95f),      .95f  },
	{ "wood",  vec3(.46f, .31f, .17f),      .95f  },
	{ "leaves",vec3(.18f, .38f, .16f),      .98f  },
	{ "planks",vec3(.63f, .47f, .27f),      .94f  },
	{ "dry grass", vec3(.56f, .48f, .20f),  .98f  },
	{ "red sand",  vec3(.64f, .28f, .16f),  .97f  },
	{ "pine leaves", vec3(.10f, .28f, .14f),.98f  },
	{ "jungle leaves", vec3(.12f, .46f, .12f),.98f},
	{ "tall grass", vec3(.22f, .58f, .18f), .96f  },
	{ "flower", vec3(.95f, .70f, .12f),     .93f  },
	{ "torch", vec3(.95f, .55f, .18f),      .82f  },
	{ "light block", vec3(1.f, .93f, .74f), .96f  },
};

inline bool world_block_solid(uint8 b)
{
	return b != WORLD_AIR && b != WORLD_TALL_GRASS && b != WORLD_FLOWER;
}

inline bool world_block_decoration(uint8 b)
{
	return b == WORLD_TALL_GRASS || b == WORLD_FLOWER;
}

inline bool world_block_emissive(uint8 b)
{
	return b == WORLD_TORCH || b == WORLD_LIGHT;
}

inline vec3 world_block_light_color(uint8 b)
{
	if (b == WORLD_TORCH) return vec3(1.00f, .55f, .18f);
	if (b == WORLD_LIGHT) return vec3(1.00f, .88f, .68f);
	return vec3(0.f);
}

inline float world_block_light_power(uint8 b)
{
	if (b == WORLD_TORCH) return 12.0f;
	if (b == WORLD_LIGHT) return 30.0f;
	return 0.f;
}

inline float world_block_light_radius(uint8 b)
{
	if (b == WORLD_TORCH) return 8.0f;
	if (b == WORLD_LIGHT) return 12.0f;
	return 0.f;
}

const vec3 WORLD_WATER_ALBEDO = vec3(.045f, .135f, .245f);

enum World_Biome
{
	BIOME_OCEAN = 0,
	BIOME_BEACH,
	BIOME_PLAINS,
	BIOME_FOREST,
	BIOME_TAIGA,
	BIOME_JUNGLE,
	BIOME_SAVANNA,
	BIOME_SWAMP,
	BIOME_DESERT,
	BIOME_BADLANDS,
	BIOME_SNOWY,
	BIOME_MOUNTAIN,
	BIOME_COUNT
};

static const char* WORLD_BIOME_NAMES[BIOME_COUNT] = {
	"ocean", "beach", "plains", "forest", "taiga", "jungle", "savanna",
	"swamp", "desert", "badlands", "snowy", "mountain"
};

struct World_Config
{
	uint  seed;
	float sea_level;
	float continent_scale;
	float continent_amplitude;
	float erosion_scale;
	float ridge_scale;
	float mountain_amplitude;
	float detail_scale;
	float detail_amplitude;
	float warp;
	bool  water;
};

World_Config world_default_config()
{
	World_Config c = {};
	c.seed                = 1337;
	c.sea_level           = 38.f;
	c.continent_scale     = 260.f;
	c.continent_amplitude = 20.f;
	c.erosion_scale       = 105.f;
	c.ridge_scale         = 72.f;
	c.mountain_amplitude  = 34.f;
	c.detail_scale        = 24.f;
	c.detail_amplitude    = 4.5f;
	c.warp                = 14.f;
	c.water               = true;
	return c;
}

static World_Config world_config = world_default_config();

struct World_Chunk
{
	int  cx, cz;
	bool used;
	bool dirty;      // mesh needs a rebuild
	uint8* blocks;   // WORLD_CHUNK^2 * WORLD_HEIGHT
	uint8* heights;  // top solid block per column

	// packed chunk geometry : one vertex range and one index range per chunk
	uint region_vert, region_vert_cap;
	uint region_indx, region_indx_cap;

	uint mesh[WORLD_BLOCK_COUNT];
	uint type_faces[WORLD_BLOCK_COUNT]; // faces in each material mesh
	uint faces;      // total visible faces
	uint vertices;   // total vertices
};

// one free byte range in the chunk arena. ranges are kept sorted by offset and
// coalesced on release, so exploring for a while leaves one large free block
struct World_Free_Range
{
	uint offset, size;
};

struct World
{
	World_Chunk chunks[WORLD_MAX_CHUNKS];
	World_Chunk* grid[WORLD_MAX_CHUNKS]; // view-relative lookup, null = unloaded

	int  center_cx, center_cz;
	uint num_chunks;
	bool built;
	bool need_compact; // arena ran out of room : rebuild the world buffer
	bool stream_pending; // generation/meshing still filling in the new view

	// renderer buffer marks, filled by the scene before a build
	uint mesh_mark, geom_mark, indx_mark;
	uint water_mesh_mark, water_geom_mark, water_indx_mark;
	uint first_prop;

	// chunk geometry arena : lives after geom_mark / indx_mark in the static
	// buffer. mesh_info slots are reserved up front, one block per chunk pool
	// slot, so a chunk can be remeshed in place without disturbing the rest
	bool arena_ready;
	uint arena_mesh_base;
	uint arena_geom_base, arena_geom_end;
	uint arena_indx_base, arena_indx_end;
	World_Free_Range free_geom[WORLD_MAX_CHUNKS + 16];
	World_Free_Range free_indx[WORLD_MAX_CHUNKS + 16];
	uint num_free_geom, num_free_indx;

	uint collider_body;
	uint water_mesh;
	float water_time;
	float render_offset_y; // terrain lab shifts the world so its sea line sits at y 0

	uint solid_blocks;
	uint faces;
	uint vertices;
};

// ---- noise -----------------------------------------------------------------

inline int world_floor(float v) { return (int)floorf(v); }

inline int world_chunk_coord(float v)
{
	int c = (int)floorf(v);
	return (c >= 0) ? c / WORLD_CHUNK : (c - (WORLD_CHUNK - 1)) / WORLD_CHUNK;
}

uint world_hash2(int x, int z, uint seed)
{
	uint h = (uint)x * 374761393u + (uint)z * 668265263u + seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

float world_rand01(int x, int z, uint seed)
{
	return (float)(world_hash2(x, z, seed) & 0x00ffffffu) / 16777215.f;
}

float world_value_noise(float x, float z, uint seed)
{
	int xi = world_floor(x);
	int zi = world_floor(z);
	float tx = x - (float)xi;
	float tz = z - (float)zi;

	float u = tx * tx * (3.f - 2.f * tx);
	float v = tz * tz * (3.f - 2.f * tz);

	float a = world_rand01(xi,     zi,     seed);
	float b = world_rand01(xi + 1, zi,     seed);
	float c = world_rand01(xi,     zi + 1, seed);
	float d = world_rand01(xi + 1, zi + 1, seed);

	return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}

float world_fbm(float x, float z, int octaves, float lacunarity, float gain, uint seed)
{
	float sum = 0.f, amplitude = .5f, norm = 0.f;
	for (int i = 0; i < octaves; i++)
	{
		sum += (world_value_noise(x, z, seed + (uint)i * 101u) * 2.f - 1.f) * amplitude;
		norm += amplitude;
		x *= lacunarity;
		z *= lacunarity;
		amplitude *= gain;
	}
	return sum / fmaxf(norm, 1e-6f);
}

// folded bands : one at the crest, zero in the valley. the modern minecraft
// peaks-and-valleys term is the same 1 - |noise| fold
float world_ridged(float x, float z, int octaves, float lacunarity, float gain, uint seed)
{
	float sum = 0.f, amplitude = .5f, norm = 0.f;
	for (int i = 0; i < octaves; i++)
	{
		float n = world_value_noise(x, z, seed + (uint)i * 131u);
		n = 1.f - fabsf(n * 2.f - 1.f);
		sum += n * n * amplitude;
		norm += amplitude;
		x *= lacunarity;
		z *= lacunarity;
		amplitude *= gain;
	}
	return sum / fmaxf(norm, 1e-6f);
}

uint world_hash3(int x, int y, int z, uint seed)
{
	uint h = (uint)x * 374761393u + (uint)y * 2246822519u + (uint)z * 668265263u + seed;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

float world_rand3(int x, int y, int z, uint seed)
{
	return (float)(world_hash3(x, y, z, seed) & 0x00ffffffu) / 16777215.f;
}

float world_value_noise3(float x, float y, float z, uint seed)
{
	int xi = world_floor(x), yi = world_floor(y), zi = world_floor(z);
	float tx = x - (float)xi, ty = y - (float)yi, tz = z - (float)zi;
	float u = tx * tx * (3.f - 2.f * tx);
	float v = ty * ty * (3.f - 2.f * ty);
	float w = tz * tz * (3.f - 2.f * tz);

	auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
	float c000 = world_rand3(xi,     yi,     zi,     seed);
	float c100 = world_rand3(xi + 1, yi,     zi,     seed);
	float c010 = world_rand3(xi,     yi + 1, zi,     seed);
	float c110 = world_rand3(xi + 1, yi + 1, zi,     seed);
	float c001 = world_rand3(xi,     yi,     zi + 1, seed);
	float c101 = world_rand3(xi + 1, yi,     zi + 1, seed);
	float c011 = world_rand3(xi,     yi + 1, zi + 1, seed);
	float c111 = world_rand3(xi + 1, yi + 1, zi + 1, seed);

	float x00 = lerp(c000, c100, u), x10 = lerp(c010, c110, u);
	float x01 = lerp(c001, c101, u), x11 = lerp(c011, c111, u);
	return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}

float world_fbm3(float x, float y, float z, int octaves, float lacunarity, float gain, uint seed)
{
	float sum = 0.f, amplitude = .5f, norm = 0.f;
	for (int i = 0; i < octaves; i++)
	{
		sum += (world_value_noise3(x, y, z, seed + (uint)i * 157u) * 2.f - 1.f) * amplitude;
		norm += amplitude;
		x *= lacunarity; y *= lacunarity; z *= lacunarity;
		amplitude *= gain;
	}
	return sum / fmaxf(norm, 1e-6f);
}

float world_temperature(const World_Config& c, float x, float z)
{
	return world_fbm(x / 420.f, z / 420.f, 3, 2.f, .5f, c.seed + 71u);
}

float world_humidity(const World_Config& c, float x, float z)
{
	return world_fbm(x / 360.f + 17.3f, z / 360.f - 9.1f, 3, 2.f, .5f, c.seed + 83u);
}

// ---- height ----------------------------------------------------------------

float world_height_at(const World_Config& c, float x, float z)
{
	float wx = x + world_fbm(x * .008f, z * .008f, 3, 2.f, .5f, c.seed + 17u) * c.warp;
	float wz = z + world_fbm(x * .008f + 43.7f, z * .008f - 21.3f, 3, 2.f, .5f, c.seed + 23u) * c.warp;

	float continent = world_fbm(wx / c.continent_scale, wz / c.continent_scale,
	                            5, 2.f, .5f, c.seed + 1u);
	float erosion = world_fbm(wx / c.erosion_scale + 31.1f, wz / c.erosion_scale - 17.7f,
	                          4, 2.1f, .5f, c.seed + 2u);
	float ridges = world_ridged(wx / c.ridge_scale, wz / c.ridge_scale,
	                            4, 2.f, .5f, c.seed + 3u);
	float detail = world_fbm(wx / c.detail_scale, wz / c.detail_scale,
	                         3, 2.f, .5f, c.seed + 4u);

	float inland = fminf(fmaxf(continent * 1.15f + .10f, 0.f), 1.f);
	float relief = fminf(fmaxf(1.f - (erosion * .5f + .5f), 0.f), 1.f);
	float base = c.sea_level + 7.f + continent * c.continent_amplitude + inland * 5.f;
	float mountains = powf(ridges, 1.8f) * c.mountain_amplitude * relief;
	float hills = detail * c.detail_amplitude * (.25f + .75f * relief);

	float h = base + mountains + hills;
	return fminf(fmaxf(h, 4.f), (float)(WORLD_HEIGHT - 6));
}

World_Biome world_biome_at(int h, int slope, float temp, float humid, const World_Config& c)
{
	if (h <= (int)c.sea_level - 3) return BIOME_OCEAN;
	if (h <= (int)c.sea_level + 2) return BIOME_BEACH;
	if (h > (int)c.sea_level + 34 || (temp < -.35f && h > (int)c.sea_level + 8)) return BIOME_SNOWY;
	if (slope >= 4 || h > (int)c.sea_level + 24) return BIOME_MOUNTAIN;
	if (temp > .30f && humid < .06f && h > (int)c.sea_level + 8) return BIOME_BADLANDS;
	if (temp > .24f && humid < .12f) return BIOME_DESERT;
	if (temp > .22f && humid < .22f) return BIOME_SAVANNA;
	if (humid > .30f && temp > .15f) return BIOME_JUNGLE;
	if (humid > .22f && h < (int)c.sea_level + 10) return BIOME_SWAMP;
	if (temp < -.12f) return BIOME_TAIGA;
	if (humid > .12f) return BIOME_FOREST;
	return BIOME_PLAINS;
}

uint8 world_surface_block(World_Biome biome, const World_Config& c)
{
	switch (biome)
	{
		case BIOME_OCEAN:
		case BIOME_BEACH:
		case BIOME_DESERT: return WORLD_SAND;
		case BIOME_BADLANDS: return WORLD_RED_SAND;
		case BIOME_SAVANNA: return WORLD_DRY_GRASS;
		case BIOME_SNOWY:  return WORLD_SNOW;
		case BIOME_MOUNTAIN: return WORLD_STONE;
		default: return WORLD_GRASS;
	}
}

// ---- chunk volume ----------------------------------------------------------

inline uint world_chunk_block_index(int x, int y, int z)
{
	return (uint)((y * WORLD_CHUNK + z) * WORLD_CHUNK + x);
}

void world_generate_chunk(World_Chunk* ch, int cx, int cz, const World_Config& c)
{
	const int cells = WORLD_CHUNK * WORLD_CHUNK;
	if (!ch->blocks)
	{
		ch->blocks  = Alloc(uint8, cells * WORLD_HEIGHT);
		ch->heights = Alloc(uint8, cells);
	}
	memset(ch->blocks, 0, cells * WORLD_HEIGHT);

	int base_x = cx * WORLD_CHUNK;
	int base_z = cz * WORLD_CHUNK;
	int heights[WORLD_CHUNK * WORLD_CHUNK];
	uint8 biomes[WORLD_CHUNK * WORLD_CHUNK];

	for (int z = 0; z < WORLD_CHUNK; z++)
		for (int x = 0; x < WORLD_CHUNK; x++)
		{
			float h = world_height_at(c, (float)(base_x + x) + .5f, (float)(base_z + z) + .5f);
			int hh = (int)(h + .5f);
			if (hh < 2) hh = 2;
			if (hh > WORLD_HEIGHT - 4) hh = WORLD_HEIGHT - 4;
			heights[z * WORLD_CHUNK + x] = hh;
			ch->heights[z * WORLD_CHUNK + x] = (uint8)hh;
		}

	for (int z = 0; z < WORLD_CHUNK; z++)
		for (int x = 0; x < WORLD_CHUNK; x++)
		{
			int h = heights[z * WORLD_CHUNK + x];
			int slope = 0;
			if (x > 0)                { int d = h - heights[z * WORLD_CHUNK + x - 1]; if (d < 0) d = -d; if (d > slope) slope = d; }
			if (x < WORLD_CHUNK - 1)  { int d = h - heights[z * WORLD_CHUNK + x + 1]; if (d < 0) d = -d; if (d > slope) slope = d; }
			if (z > 0)                { int d = h - heights[(z - 1) * WORLD_CHUNK + x]; if (d < 0) d = -d; if (d > slope) slope = d; }
			if (z < WORLD_CHUNK - 1)  { int d = h - heights[(z + 1) * WORLD_CHUNK + x]; if (d < 0) d = -d; if (d > slope) slope = d; }

			float temp = world_temperature(c, (float)(base_x + x) + .5f, (float)(base_z + z) + .5f);
			float humid = world_humidity(c, (float)(base_x + x) + .5f, (float)(base_z + z) + .5f);
			World_Biome biome = world_biome_at(h, slope, temp, humid, c);
			uint8 top = world_surface_block(biome, c);
			uint8 soil = (top == WORLD_SAND || top == WORLD_RED_SAND) ? top : WORLD_DIRT;
			biomes[z * WORLD_CHUNK + x] = (uint8)biome;

			for (int y = 0; y < h; y++)
			{
				uint8 block = WORLD_STONE;
				if (y == h - 1)      block = top;
				else if (y >= h - 4) block = soil;
				ch->blocks[world_chunk_block_index(x, y, z)] = block;
			}

			// two intersecting 3-D noise fields carve winding caves below the
			// surface. the top three layers stay intact so the player does not
			// spawn into a hole in the meadow
			int gx = base_x + x;
			int gz = base_z + z;
			for (int y = 4; y < h - 3; y++)
			{
				float a = world_fbm3(gx * .045f, y * .075f, gz * .045f, 2, 2.f, .5f, c.seed + 41u);
				float b = world_fbm3(gx * .045f + 17.3f, y * .075f, gz * .045f - 9.1f,
				                     2, 2.f, .5f, c.seed + 43u);
				if (fabsf(a) < .055f && fabsf(b) < .085f)
					ch->blocks[world_chunk_block_index(x, y, z)] = WORLD_AIR;
			}
		}

	// biome trees : the leaf type, trunk height and crown shape follow the
	// biome. kept two blocks from the chunk edge so a crown never spills across
	// a streamed boundary
	for (int z = 2; z < WORLD_CHUNK - 2; z++)
		for (int x = 2; x < WORLD_CHUNK - 2; x++)
		{
			int h = heights[z * WORLD_CHUNK + x];
			if (h + 10 >= WORLD_HEIGHT) continue;
			int wx = base_x + x;
			int wz = base_z + z;
			if (wx >= -12 && wx <= 12 && wz >= -12 && wz <= 12) continue; // keep the spawn clear

			World_Biome biome = (World_Biome)biomes[z * WORLD_CHUNK + x];
			if (biome == BIOME_JUNGLE && (x < 3 || x > WORLD_CHUNK - 4 ||
			                              z < 3 || z > WORLD_CHUNK - 4)) continue;
			uint chance = 0;
			switch (biome)
			{
				case BIOME_FOREST:  chance = 28; break;
				case BIOME_TAIGA:   chance = 38; break;
				case BIOME_JUNGLE:  chance = 48; break;
				case BIOME_SWAMP:   chance = 14; break;
				case BIOME_SAVANNA: chance = 6;  break;
				case BIOME_PLAINS:  chance = 7;  break;
				default: continue;
			}

			uint roll = world_hash2(wx, wz, c.seed + 909u) % 1000u;
			if (roll >= chance) continue;

			uint8 leaf = WORLD_LEAVES;
			int trunk = 4 + (int)(world_hash2(wx, wz, c.seed + 911u) % 3u);
			int crown_start = -2, crown_end = 1, crown_radius = 2;
			if (biome == BIOME_TAIGA)
			{
				leaf = WORLD_PINE_LEAVES;
				trunk = 5 + (int)(world_hash2(wx, wz, c.seed + 911u) % 3u);
				crown_start = -3; crown_end = 0;
			}
			else if (biome == BIOME_JUNGLE)
			{
				leaf = WORLD_JUNGLE_LEAVES;
				trunk = 6 + (int)(world_hash2(wx, wz, c.seed + 911u) % 4u);
				crown_start = -2; crown_end = 2; crown_radius = 3;
			}
			else if (biome == BIOME_SAVANNA)
			{
				trunk = 4 + (int)(world_hash2(wx, wz, c.seed + 911u) % 2u);
				crown_start = 0; crown_end = 1;
			}

			for (int yy = 0; yy < trunk; yy++)
				ch->blocks[world_chunk_block_index(x, h + yy, z)] = WORLD_WOOD;

			int top_y = h + trunk - 1;
			for (int ly = crown_start; ly <= crown_end; ly++)
			{
				int radius = (ly <= -1) ? crown_radius : crown_radius - 1;
				if (biome == BIOME_JUNGLE && ly <= -1) radius = 3;
				if (biome == BIOME_SAVANNA) radius = (ly == 0) ? 2 : 1;
				if (radius < 1) radius = 1;
				for (int dz = -radius; dz <= radius; dz++)
					for (int dx = -radius; dx <= radius; dx++)
					{
						if (dx * dx + dz * dz > radius * radius + 1) continue;
						int lx = x + dx, lz = z + dz;
						int by = top_y + ly;
						if (lx < 0 || lz < 0 || lx >= WORLD_CHUNK || lz >= WORLD_CHUNK) continue;
						if (by < 0 || by >= WORLD_HEIGHT) continue;
						uint index = world_chunk_block_index(lx, by, lz);
						if (ch->blocks[index] == WORLD_AIR)
							ch->blocks[index] = leaf;
					}
			}
		}

	// ground cover : tall grass and flowers on grassy surfaces. these are
	// cross-quad decorations, not solid cubes, so collision and cube culling
	// treat them as air
	for (int z = 0; z < WORLD_CHUNK; z++)
		for (int x = 0; x < WORLD_CHUNK; x++)
		{
			int h = heights[z * WORLD_CHUNK + x];
			if (h >= WORLD_HEIGHT - 1) continue;
			uint index = world_chunk_block_index(x, h, z);
			if (ch->blocks[index] != WORLD_AIR) continue;

			uint8 surface = ch->blocks[world_chunk_block_index(x, h - 1, z)];
			if (surface != WORLD_GRASS && surface != WORLD_DRY_GRASS) continue;

			World_Biome biome = (World_Biome)biomes[z * WORLD_CHUNK + x];
			uint grass_chance = 0, flower_chance = 0;
			switch (biome)
			{
				case BIOME_PLAINS:  grass_chance = 180; flower_chance = 14; break;
				case BIOME_FOREST:  grass_chance = 260; flower_chance = 16; break;
				case BIOME_TAIGA:   grass_chance = 110; flower_chance = 5;  break;
				case BIOME_JUNGLE:  grass_chance = 420; break;
				case BIOME_SWAMP:   grass_chance = 180; break;
				case BIOME_SAVANNA: grass_chance = 80;  break;
				default: continue;
			}

			uint wx = (uint)(base_x + x), wz = (uint)(base_z + z);
			uint r = world_hash2((int)wx, (int)wz, c.seed + 1201u) % 1000u;
			if (r < grass_chance)
				ch->blocks[index] = WORLD_TALL_GRASS;
			else if (r < grass_chance + flower_chance)
				ch->blocks[index] = WORLD_FLOWER;
		}

	ch->cx = cx;
	ch->cz = cz;
	ch->used = true;
}

inline World_Chunk* world_chunk_at(World* w, int cx, int cz)
{
	int gx = cx - w->center_cx + WORLD_VIEW_RADIUS;
	int gz = cz - w->center_cz + WORLD_VIEW_RADIUS;
	if (gx < 0 || gz < 0 || gx >= WORLD_VIEW_CHUNKS || gz >= WORLD_VIEW_CHUNKS) return NULL;
	return w->grid[gz * WORLD_VIEW_CHUNKS + gx];
}

uint8 world_stream_block(World* w, int x, int y, int z)
{
	if (y < 0 || y >= WORLD_HEIGHT) return WORLD_AIR;

	int cx = (x >= 0) ? x / WORLD_CHUNK : (x - (WORLD_CHUNK - 1)) / WORLD_CHUNK;
	int cz = (z >= 0) ? z / WORLD_CHUNK : (z - (WORLD_CHUNK - 1)) / WORLD_CHUNK;
	World_Chunk* ch = world_chunk_at(w, cx, cz);
	if (!ch) return WORLD_AIR;

	int lx = x - cx * WORLD_CHUNK;
	int lz = z - cz * WORLD_CHUNK;
	return ch->blocks[world_chunk_block_index(lx, y, lz)];
}

uint8 world_get_block(World* w, int x, int y, int z)
{
	return world_stream_block(w, x, y, z);
}

bool world_set_block(World* w, int x, int y, int z, uint8 block)
{
	if (y < 0 || y >= WORLD_HEIGHT) return false;

	int cx = (x >= 0) ? x / WORLD_CHUNK : (x - (WORLD_CHUNK - 1)) / WORLD_CHUNK;
	int cz = (z >= 0) ? z / WORLD_CHUNK : (z - (WORLD_CHUNK - 1)) / WORLD_CHUNK;
	World_Chunk* ch = world_chunk_at(w, cx, cz);
	if (!ch) return false;

	int lx = x - cx * WORLD_CHUNK;
	int lz = z - cz * WORLD_CHUNK;
	uint index = world_chunk_block_index(lx, y, lz);
	if (ch->blocks[index] == block) return false;
	ch->blocks[index] = block;

	int top = 0;
	for (int yy = WORLD_HEIGHT - 1; yy >= 0; yy--)
		if (world_block_solid(ch->blocks[world_chunk_block_index(lx, yy, lz)]))
		{
			top = yy + 1;
			break;
		}
	ch->heights[lz * WORLD_CHUNK + lx] = (uint8)top;
	return true;
}

struct World_Hit
{
	bool hit;
	int  x, y, z;
	int  nx, ny, nz; // face normal, pointing toward the viewer
};

// short voxel ray march, good enough for a 6 m reach and independent of the
// render raycast machinery. the previous cell gives the face normal
bool world_raycast(World* w, vec3 origin, vec3 dir, float max_dist, World_Hit* out)
{
	if (!out) return false;
	out->hit = false;

	vec3 d = normalize(dir);
	int px = world_floor(origin.x);
	int py = world_floor(origin.y);
	int pz = world_floor(origin.z);

	float step = .02f;
	for (float t = 0.f; t <= max_dist; t += step)
	{
		vec3 p = origin + d * t;
		int bx = world_floor(p.x);
		int by = world_floor(p.y);
		int bz = world_floor(p.z);
		if (bx == px && by == py && bz == pz) continue;

		if (world_stream_block(w, bx, by, bz) != WORLD_AIR)
		{
			out->hit = true;
			out->x = bx; out->y = by; out->z = bz;
			out->nx = px - bx;
			out->ny = py - by;
			out->nz = pz - bz;
			return true;
		}

		px = bx; py = by; pz = bz;
	}

	return false;
}

// ---- chunk meshing ---------------------------------------------------------

void world_face_verts(float x, float y, float z, int dir, vec3 p[4], vec3* normal)
{
	switch (dir)
	{
		case 0:
			p[0] = vec3(x + 1, y,     z    ); p[1] = vec3(x + 1, y + 1, z    );
			p[2] = vec3(x + 1, y + 1, z + 1); p[3] = vec3(x + 1, y,     z + 1);
			*normal = vec3(1, 0, 0); break;
		case 1:
			p[0] = vec3(x, y,     z + 1); p[1] = vec3(x, y + 1, z + 1);
			p[2] = vec3(x, y + 1, z    ); p[3] = vec3(x, y,     z    );
			*normal = vec3(-1, 0, 0); break;
		case 2:
			p[0] = vec3(x,     y + 1, z    ); p[1] = vec3(x,     y + 1, z + 1);
			p[2] = vec3(x + 1, y + 1, z + 1); p[3] = vec3(x + 1, y + 1, z    );
			*normal = vec3(0, 1, 0); break;
		case 3:
			p[0] = vec3(x,     y, z    ); p[1] = vec3(x + 1, y, z    );
			p[2] = vec3(x + 1, y, z + 1); p[3] = vec3(x,     y, z + 1);
			*normal = vec3(0, -1, 0); break;
		case 4:
			p[0] = vec3(x + 1, y,     z + 1); p[1] = vec3(x + 1, y + 1, z + 1);
			p[2] = vec3(x,     y + 1, z + 1); p[3] = vec3(x,     y,     z + 1);
			*normal = vec3(0, 0, 1); break;
		default:
			p[0] = vec3(x,     y,     z); p[1] = vec3(x,     y + 1, z);
			p[2] = vec3(x + 1, y + 1, z); p[3] = vec3(x + 1, y,     z);
			*normal = vec3(0, 0, -1); break;
	}
}

// ---- chunk geometry arena --------------------------------------------------
// chunks keep their geometry in one vertex range and one index range inside the
// static buffer. the ranges are allocated from a free list, so a chunk that
// leaves the view gives its bytes back to the next one that arrives. when a
// chunk is remeshed its ranges are updated in place instead of rebuilding every
// other chunk in the view

inline uint world_align_up(uint value, uint alignment)
{
	return (value + alignment - 1u) & ~(alignment - 1u);
}

void world_free_insert(World_Free_Range* list, uint* count, uint offset, uint size)
{
	if (size == 0) return;
	if (*count >= WORLD_MAX_CHUNKS + 16) return; // caller leaked a range : keep going

	uint i = 0;
	while (i < *count && list[i].offset < offset) i++;
	for (uint j = *count; j > i; j--) list[j] = list[j - 1];
	list[i].offset = offset;
	list[i].size   = size;
	(*count)++;

	if (i + 1 < *count && list[i].offset + list[i].size == list[i + 1].offset)
	{
		list[i].size += list[i + 1].size;
		for (uint j = i + 1; j + 1 < *count; j++) list[j] = list[j + 1];
		(*count)--;
	}
	if (i > 0 && list[i - 1].offset + list[i - 1].size == list[i].offset)
	{
		list[i - 1].size += list[i].size;
		for (uint j = i; j + 1 < *count; j++) list[j] = list[j + 1];
		(*count)--;
	}
}

bool world_free_take(World_Free_Range* list, uint* count, uint* heap_end, uint size,
                     uint max_size, uint* offset, uint* capacity)
{
	size = world_align_up(size, 32u);

	for (uint i = 0; i < *count; i++)
	{
		if (list[i].size < size) continue;

		*offset   = list[i].offset;
		*capacity = size;
		list[i].offset += size;
		list[i].size   -= size;
		if (list[i].size == 0)
		{
			for (uint j = i; j + 1 < *count; j++) list[j] = list[j + 1];
			(*count)--;
		}
		return true;
	}

	if (*heap_end + size <= max_size)
	{
		*offset   = *heap_end;
		*capacity = size;
		*heap_end += size;
		return true;
	}
	return false;
}

void world_arena_reset(World* w, DrawBuffer* db)
{
	w->arena_geom_base = db->geom_size;
	w->arena_geom_end  = db->geom_size;
	w->arena_indx_base = db->indx_size;
	w->arena_indx_end  = db->indx_size;
	w->num_free_geom   = 0;
	w->num_free_indx   = 0;

	// one mesh_info block per chunk pool slot, so remeshing a chunk only rewrites
	// its own entries. ids stay stable across updates, which keeps props grouped
	int base = db->reserve_meshes(WORLD_MAX_CHUNKS * WORLD_BLOCK_COUNT);
	if (base < 0)
	{
		console->add_entry("voxel arena mesh slots full", FIXME, RNDR);
		w->arena_ready = false;
		return;
	}

	w->arena_mesh_base = (uint)base;
	w->arena_ready     = true;
}

bool world_chunk_alloc(World* w, DrawBuffer* db, World_Chunk* ch, uint vert_bytes, uint indx_bytes)
{
	if (vert_bytes > ch->region_vert_cap)
	{
		if (ch->region_vert_cap)
			world_free_insert(w->free_geom, &w->num_free_geom,
			                  ch->region_vert, ch->region_vert_cap);

		uint offset = 0, capacity = 0;
		if (!world_free_take(w->free_geom, &w->num_free_geom, &w->arena_geom_end,
		                     vert_bytes, db->max_buffer_size, &offset, &capacity))
		{
			ch->region_vert_cap = 0;
			return false;
		}
		ch->region_vert     = offset;
		ch->region_vert_cap = capacity;
	}

	if (indx_bytes > ch->region_indx_cap)
	{
		if (ch->region_indx_cap)
			world_free_insert(w->free_indx, &w->num_free_indx,
			                  ch->region_indx, ch->region_indx_cap);

		uint offset = 0, capacity = 0;
		if (!world_free_take(w->free_indx, &w->num_free_indx, &w->arena_indx_end,
		                     indx_bytes, db->max_indx_size, &offset, &capacity))
		{
			ch->region_indx_cap = 0;
			return false;
		}
		ch->region_indx     = offset;
		ch->region_indx_cap = capacity;
	}

	return true;
}

void world_chunk_release(World* w, World_Chunk* ch, DrawBuffer* db)
{
	if (ch->region_vert_cap)
		world_free_insert(w->free_geom, &w->num_free_geom, ch->region_vert, ch->region_vert_cap);
	if (ch->region_indx_cap)
		world_free_insert(w->free_indx, &w->num_free_indx, ch->region_indx, ch->region_indx_cap);

	ch->region_vert = ch->region_vert_cap = 0;
	ch->region_indx = ch->region_indx_cap = 0;

	if (w->arena_ready)
	{
		uint pool  = (uint)(ch - w->chunks);
		uint entry = w->arena_mesh_base + pool * WORLD_BLOCK_COUNT;
		for (uint b = 0; b < WORLD_BLOCK_COUNT; b++)
		{
			ch->mesh[b]       = 0;
			ch->type_faces[b] = 0;
			db->mesh_info[entry + b] = {};
		}
	}

	ch->faces = 0;
	ch->vertices = 0;
}

bool world_chunk_meshes(World* w, World_Chunk* ch, GameRenderer* renderer)
{
	if (!w->arena_ready) return false;

	DrawBuffer& db = renderer->buffer(MODE_STATIC_UV);
	uint counts[WORLD_BLOCK_COUNT] = {};
	uint total = 0;

	for (int y = 0; y < WORLD_HEIGHT; y++)
		for (int z = 0; z < WORLD_CHUNK; z++)
			for (int x = 0; x < WORLD_CHUNK; x++)
			{
				uint8 b = ch->blocks[world_chunk_block_index(x, y, z)];
				if (b == WORLD_AIR) continue;

				// decorations are two crossed quads, not six cube faces; their
				// neighbours do not cull them and they do not cull the cubes
				if (world_block_decoration(b))
				{
					counts[b] += 2;
					total += 2;
					continue;
				}

				int gx = ch->cx * WORLD_CHUNK + x;
				int gz = ch->cz * WORLD_CHUNK + z;
				for (int dir = 0; dir < 6; dir++)
				{
					int dx = (dir == 0) - (dir == 1);
					int dy = (dir == 2) - (dir == 3);
					int dz = (dir == 4) - (dir == 5);
					if (dir == 3 && y == 0) continue;
					if (!world_block_solid(world_stream_block(w, gx + dx, y + dy, gz + dz)))
					{
						counts[b]++;
						total++;
					}
				}
			}

	uint total_verts = total * 4;
	uint total_idx   = total * 6;
	uint vert_bytes  = total_verts * (uint)sizeof(MeshVertexData);
	uint indx_bytes  = total_idx   * (uint)sizeof(uint);

	if (!world_chunk_alloc(w, &db, ch, vert_bytes, indx_bytes))
		return false;

	if (total > 0)
	{
		MeshVertexData* verts = Alloc(MeshVertexData, total_verts);
		uint* indices = Alloc(uint, total_idx);

		uint type_vert[WORLD_BLOCK_COUNT] = {};
		uint cursor = 0;
		for (uint b = 1; b < WORLD_BLOCK_COUNT; b++)
		{
			type_vert[b] = cursor;
			cursor += counts[b] * 4;
		}

		uint index_cursor = 0;
		for (uint b = 1; b < WORLD_BLOCK_COUNT; b++)
		{
			if (counts[b] == 0) continue;

			uint v = type_vert[b];

			if (world_block_decoration(b))
			{
				auto quad = [&](vec3 a, vec3 bb, vec3 cc, vec3 dd) {
					uint base = v;
					verts[v].position = a;  verts[v].normal = vec3(0, 1, 0); verts[v].uv = vec2(0, 0); v++;
					verts[v].position = bb; verts[v].normal = vec3(0, 1, 0); verts[v].uv = vec2(1, 0); v++;
					verts[v].position = cc; verts[v].normal = vec3(0, 1, 0); verts[v].uv = vec2(1, 1); v++;
					verts[v].position = dd; verts[v].normal = vec3(0, 1, 0); verts[v].uv = vec2(0, 1); v++;
					indices[index_cursor++] = base + 0;
					indices[index_cursor++] = base + 1;
					indices[index_cursor++] = base + 2;
					indices[index_cursor++] = base + 0;
					indices[index_cursor++] = base + 2;
					indices[index_cursor++] = base + 3;
				};

				for (int y = 0; y < WORLD_HEIGHT; y++)
					for (int z = 0; z < WORLD_CHUNK; z++)
						for (int x = 0; x < WORLD_CHUNK; x++)
						{
							if (ch->blocks[world_chunk_block_index(x, y, z)] != b) continue;
							float x0 = (float)x + .28f, x1 = (float)x + .72f;
							float z0 = (float)z + .28f, z1 = (float)z + .72f;
							float y0 = (float)y, y1 = (float)y + .55f;
							quad(vec3(x0, y0, z0), vec3(x0, y1, z0),
							     vec3(x1, y1, z1), vec3(x1, y0, z1));
							quad(vec3(x0, y0, z1), vec3(x0, y1, z1),
							     vec3(x1, y1, z0), vec3(x1, y0, z0));
						}
				continue;
			}

			for (int y = 0; y < WORLD_HEIGHT; y++)
				for (int z = 0; z < WORLD_CHUNK; z++)
					for (int x = 0; x < WORLD_CHUNK; x++)
					{
						if (ch->blocks[world_chunk_block_index(x, y, z)] != b) continue;
						int gx = ch->cx * WORLD_CHUNK + x;
						int gz = ch->cz * WORLD_CHUNK + z;

						for (int dir = 0; dir < 6; dir++)
						{
							int dx = (dir == 0) - (dir == 1);
							int dy = (dir == 2) - (dir == 3);
							int dz = (dir == 4) - (dir == 5);
							if (dir == 3 && y == 0) continue;
							if (world_block_solid(world_stream_block(w, gx + dx, y + dy, gz + dz))) continue;

							vec3 p[4];
							vec3 normal;
							world_face_verts((float)x, (float)y, (float)z, dir, p, &normal);

							uint base = v;
							for (uint k = 0; k < 4; k++)
							{
								verts[v].position = p[k];
								verts[v].normal   = normal;
								verts[v].uv       = vec2((k == 1 || k == 2) ? 1.f : 0.f,
								                         (k >= 2) ? 1.f : 0.f);
								v++;
							}

							indices[index_cursor++] = base + 0;
							indices[index_cursor++] = base + 1;
							indices[index_cursor++] = base + 2;
							indices[index_cursor++] = base + 0;
							indices[index_cursor++] = base + 2;
							indices[index_cursor++] = base + 3;
						}
					}
		}

		db.write_geometry(ch->region_vert, total_verts, verts);
		db.write_indices(ch->region_indx, total_idx, indices);

		free(verts);
		free(indices);
	}

	uint pool         = (uint)(ch - w->chunks);
	uint entry        = w->arena_mesh_base + pool * WORLD_BLOCK_COUNT;
	uint base_vertex  = ch->region_vert / (uint)sizeof(MeshVertexData);
	uint index_cursor = 0;

	ch->faces    = total;
	ch->vertices = total_verts;

	for (uint b = 1; b < WORLD_BLOCK_COUNT; b++)
	{
		ch->type_faces[b] = counts[b];

		auto& mi = db.mesh_info[entry + b];
		if (counts[b] == 0)
		{
			ch->mesh[b] = 0;
			mi = {};
			continue;
		}

		if (mi.mesh_id == 0) mi.mesh_id = ++renderer->next_mesh_id;
		mi.tx_id         = 0;
		mi.num_vertices  = counts[b] * 4;
		mi.num_indices   = counts[b] * 6;
		mi.num_instances = 0;
		mi.base_vertex   = base_vertex;
		mi.base_index    = ch->region_indx + index_cursor * (uint)sizeof(uint);
		mi.base_instance = 0;

		ch->mesh[b] = mi.mesh_id;
		index_cursor += counts[b] * 6;
	}

	return true;
}

// mark the chunk holding a block, and any chunk it touches across a border, for
// a remesh. used by the break/place edit path so a single block change does not
// cost a full world rebuild
void world_mark_chunk_dirty(World* w, int cx, int cz)
{
	World_Chunk* ch = world_chunk_at(w, cx, cz);
	if (ch) ch->dirty = true;
}

void world_mark_block_dirty(World* w, int x, int z)
{
	int cx = world_chunk_coord((float)x);
	int cz = world_chunk_coord((float)z);
	int lx = x - cx * WORLD_CHUNK;
	int lz = z - cz * WORLD_CHUNK;

	world_mark_chunk_dirty(w, cx, cz);
	if (lx == 0)                 world_mark_chunk_dirty(w, cx - 1, cz);
	if (lx == WORLD_CHUNK - 1)   world_mark_chunk_dirty(w, cx + 1, cz);
	if (lz == 0)                 world_mark_chunk_dirty(w, cx, cz - 1);
	if (lz == WORLD_CHUNK - 1)   world_mark_chunk_dirty(w, cx, cz + 1);
}

void world_refresh_stats(World* w)
{
	w->faces = 0;
	w->vertices = 0;
	w->solid_blocks = 0;
	for (uint i = 0; i < WORLD_MAX_CHUNKS; i++)
	{
		World_Chunk& ch = w->chunks[i];
		if (!ch.used) continue;
		w->faces += ch.faces;
		w->vertices += ch.vertices;
		for (int k = 0; k < WORLD_CHUNK * WORLD_CHUNK; k++) w->solid_blocks += ch.heights[k];
	}
}

bool world_remesh_dirty(World* w, GameRenderer* renderer)
{
	for (uint i = 0; i < WORLD_MAX_CHUNKS; i++)
	{
		World_Chunk& ch = w->chunks[i];
		if (!ch.used || !ch.dirty) continue;
		if (!world_chunk_meshes(w, &ch, renderer))
		{
			w->need_compact = true;
			return false;
		}
		ch.dirty = false;
	}
	world_refresh_stats(w);
	return true;
}

// ---- streaming -------------------------------------------------------------

World_Chunk* world_find_chunk(World* w, int cx, int cz)
{
	for (uint i = 0; i < WORLD_MAX_CHUNKS; i++)
		if (w->chunks[i].used && w->chunks[i].cx == cx && w->chunks[i].cz == cz)
			return &w->chunks[i];
	return NULL;
}

World_Chunk* world_get_chunk(World* w, int cx, int cz, const World_Config& c)
{
	World_Chunk* ch = world_find_chunk(w, cx, cz);
	if (ch) return ch;

	for (uint i = 0; i < WORLD_MAX_CHUNKS; i++)
	{
		if (w->chunks[i].used) continue;
		ch = &w->chunks[i];
		world_generate_chunk(ch, cx, cz, c);
		w->num_chunks++;
		return ch;
	}

	console->add_entry("voxel chunk pool full", FIXME, RNDR);
	return NULL;
}

void world_unload_chunk(World_Chunk* ch)
{
	free(ch->blocks);
	free(ch->heights);
	ch->blocks = NULL;
	ch->heights = NULL;
	ch->used = false;
}

float* world_build_heightfield(World* w, const World_Config& c, int* n_out, vec3* origin_out)
{
	// Bullet samples the field on a square grid centred on the body. one sample
	// per block over the loaded view gives the player a continuous surface and
	// avoids the one-sided triangle winding traps of a top-face tri mesh
	int n = WORLD_VIEW_CHUNKS * WORLD_CHUNK + 1;
	float* heights = Alloc(float, n * n);
	float cx = (float)(w->center_cx * WORLD_CHUNK) + WORLD_CHUNK * .5f;
	float cz = (float)(w->center_cz * WORLD_CHUNK) + WORLD_CHUNK * .5f;
	float half = (float)(n - 1) * .5f;

	for (int z = 0; z < n; z++)
		for (int x = 0; x < n; x++)
		{
			float wx = cx - half + (float)x;
			float wz = cz - half + (float)z;

			// prefer the live chunk height so breaking and placing blocks changes
			// collision immediately. outside the loaded view fall back to the
			// generator so the edge of the field still has a surface
			int bx = (int)floorf(wx);
			int bz = (int)floorf(wz);
			int ccx = (bx >= 0) ? bx / WORLD_CHUNK : (bx - (WORLD_CHUNK - 1)) / WORLD_CHUNK;
			int ccz = (bz >= 0) ? bz / WORLD_CHUNK : (bz - (WORLD_CHUNK - 1)) / WORLD_CHUNK;
			World_Chunk* ch = world_chunk_at(w, ccx, ccz);

			int hh;
			if (ch)
				hh = ch->heights[(bz - ccz * WORLD_CHUNK) * WORLD_CHUNK + (bx - ccx * WORLD_CHUNK)];
			else
			{
				float h = world_height_at(c, wx + .5f, wz + .5f);
				hh = (int)(h + .5f);
				if (hh < 2) hh = 2;
				if (hh > WORLD_HEIGHT - 4) hh = WORLD_HEIGHT - 4;
			}
			heights[z * n + x] = (float)hh;
		}

	*n_out = n;
	*origin_out = vec3(cx, 0.f, cz);
	return heights;
}

Mesh_Data world_water_plane(float x0, float z0, float x1, float z1, float y)
{
	Mesh_Data water = {};
	water.num_vertices = 4;
	water.num_indices  = 6;
	water.positions = Alloc(vec3, 4);
	water.normals   = Alloc(vec3, 4);
	water.uvs       = Alloc(vec2, 4);
	water.indices   = Alloc(uint, 6);

	water.positions[0] = vec3(x0, y, z0);
	water.positions[1] = vec3(x1, y, z0);
	water.positions[2] = vec3(x1, y, z1);
	water.positions[3] = vec3(x0, y, z1);
	for (uint i = 0; i < 4; i++) { water.normals[i] = vec3(0, 1, 0); water.uvs[i] = vec2(0); }
	water.indices[0] = 0; water.indices[1] = 1; water.indices[2] = 2;
	water.indices[3] = 0; water.indices[4] = 2; water.indices[5] = 3;
	return water;
}

// stream the view around a new centre. a full build (first load, or after the
// arena ran out of room) resets the chunk arena and remeshes everything; a
// normal crossing only rebuilds the chunks whose neighbour set changed, so the
// rest of the loaded world stays on the gpu untouched
void world_stream(World* w, int center_cx, int center_cz, const World_Config& c,
                  GameRenderer* renderer, Physics* phys, bool collision)
{
	DrawBuffer& db = renderer->buffer(MODE_STATIC_UV);
	bool full = !w->built || w->need_compact;
	w->need_compact = false;

	// drop chunks that left the view. the chunks still inside lose a neighbour
	// and must re-check their outer faces
	for (uint i = 0; i < WORLD_MAX_CHUNKS; i++)
	{
		World_Chunk& ch = w->chunks[i];
		if (!ch.used) continue;
		if (ch.cx < center_cx - WORLD_VIEW_RADIUS || ch.cx > center_cx + WORLD_VIEW_RADIUS ||
		    ch.cz < center_cz - WORLD_VIEW_RADIUS || ch.cz > center_cz + WORLD_VIEW_RADIUS)
		{
			if (!full)
			{
				int dx4[4] = { 1, -1, 0, 0 };
				int dz4[4] = { 0, 0, 1, -1 };
				for (int d = 0; d < 4; d++)
				{
					World_Chunk* nb = world_find_chunk(w, ch.cx + dx4[d], ch.cz + dz4[d]);
					if (nb) nb->dirty = true;
				}
				world_chunk_release(w, &ch, &db);
			}
			world_unload_chunk(&ch);
			w->num_chunks--;
		}
	}

	if (full)
	{
		world_arena_reset(w, &db);
		// the buffer was rewound : every surviving chunk needs fresh ranges
		for (uint i = 0; i < WORLD_MAX_CHUNKS; i++)
		{
			World_Chunk& ch = w->chunks[i];
			if (!ch.used) continue;
			ch.region_vert = ch.region_vert_cap = 0;
			ch.region_indx = ch.region_indx_cap = 0;
			ch.dirty = true;
		}
	}

	w->center_cx = center_cx;
	w->center_cz = center_cz;
	memset(w->grid, 0, sizeof(w->grid));

	World_Chunk* fresh[WORLD_MAX_CHUNKS];
	uint num_fresh = 0;
	bool incomplete = false;

	for (int dz = -WORLD_VIEW_RADIUS; dz <= WORLD_VIEW_RADIUS; dz++)
		for (int dx = -WORLD_VIEW_RADIUS; dx <= WORLD_VIEW_RADIUS; dx++)
		{
			int cx = center_cx + dx;
			int cz = center_cz + dz;
			bool is_new = world_find_chunk(w, cx, cz) == NULL;

			// a crossing brings a whole strip of new chunks. generate only a few
			// per frame and leave the rest for the next update
			if (is_new && !full && num_fresh >= (uint)WORLD_GEN_BUDGET)
			{
				incomplete = true;
				continue;
			}

			World_Chunk* ch = world_get_chunk(w, cx, cz, c);
			w->grid[(dz + WORLD_VIEW_RADIUS) * WORLD_VIEW_CHUNKS + (dx + WORLD_VIEW_RADIUS)] = ch;

			if (!ch) continue;
			if (is_new)
			{
				ch->region_vert = ch->region_vert_cap = 0;
				ch->region_indx = ch->region_indx_cap = 0;
				ch->dirty = true;
				fresh[num_fresh++] = ch;
			}
		}

	// a newly loaded chunk removes the outer faces its neighbours had drawn
	for (uint i = 0; i < num_fresh; i++)
	{
		World_Chunk* ch = fresh[i];
		int dx4[4] = { 1, -1, 0, 0 };
		int dz4[4] = { 0, 0, 1, -1 };
		for (int d = 0; d < 4; d++)
		{
			World_Chunk* nb = world_chunk_at(w, ch->cx + dx4[d], ch->cz + dz4[d]);
			if (nb) nb->dirty = true;
		}
	}

	// remesh only the dirty chunks, a few per frame
	bool failed = false;
	uint meshed = 0;
	for (uint i = 0; i < WORLD_MAX_CHUNKS; i++)
	{
		World_Chunk& ch = w->chunks[i];
		if (!ch.used || !ch.dirty) continue;

		if (!full && meshed >= (uint)WORLD_MESH_BUDGET)
		{
			incomplete = true;
			break;
		}

		if (!world_chunk_meshes(w, &ch, renderer))
		{
			failed = true;
			break;
		}
		ch.dirty = false;
		meshed++;
	}

	if (failed)
	{
		if (!full)
		{
			w->need_compact = true;
			return; // the next scene update rebuilds the whole buffer
		}

		// a full rebuild could not fit every chunk : keep what landed instead of
		// retrying forever. this is a hard limit, not a frame hitch
		console->add_entry("voxel chunk arena exhausted", FIXME, RNDR);
		incomplete = false;
	}

	// view totals from the per-chunk caches
	world_refresh_stats(w);

	// water is one plane over the loaded square. it follows the stream centre so
	// the sea never ends at the render edge
	DrawBuffer& wdb = renderer->buffer(MODE_WATER);
	wdb.rewind(w->water_mesh_mark, w->water_geom_mark, w->water_indx_mark);
	w->water_mesh = 0;
	if (c.water)
	{
		// the sea plane is much wider than the loaded block view : the horizon
		// stays water while the land streams in and out underneath it
		float cx = (float)(center_cx * WORLD_CHUNK + WORLD_CHUNK / 2);
		float cz = (float)(center_cz * WORLD_CHUNK + WORLD_CHUNK / 2);
		float half = 2048.f;
		w->water_mesh = renderer->add_mesh_water(
			world_water_plane(cx - half, cz - half, cx + half, cz + half,
			                  c.sea_level - .10f + w->render_offset_y));
	}

	if (collision && phys)
	{
		if (w->collider_body)
		{
			phys->remove_body(w->collider_body);
			w->collider_body = 0;
		}

		int n = 0;
		vec3 origin;
		float* heights = world_build_heightfield(w, c, &n, &origin);
		w->collider_body = phys->add_heightfield(heights, n, 1.f, origin, 0.f, (float)WORLD_HEIGHT);
	}

	w->stream_pending = incomplete;
	w->built = true;
}

void world_shutdown(World* w, Physics* phys)
{
	if (w->collider_body && phys)
	{
		phys->remove_body(w->collider_body);
		w->collider_body = 0;
	}
	for (uint i = 0; i < WORLD_MAX_CHUNKS; i++)
		if (w->chunks[i].used) world_unload_chunk(&w->chunks[i]);
	w->num_chunks = 0;
	w->built = false;
	w->need_compact = false;
	w->stream_pending = false;
	w->arena_ready = false;
}

bool world_spawn(const World* w, const World_Config& c, vec3* out)
{
	int base_x = w->center_cx * WORLD_CHUNK + WORLD_CHUNK / 2;
	int base_z = w->center_cz * WORLD_CHUNK + WORLD_CHUNK / 2;

	vec3 best = vec3((float)base_x + .5f, c.sea_level + 12.f, (float)base_z + .5f);
	int best_score = 1000;

	// walk rings out from the centre. a perfect site is a level 3 x 3 patch with
	// a walkable surface and no tree in it; a merely gentle site is the fallback
	for (int radius = 0; radius <= 40; radius++)
	{
		for (int dz = -radius; dz <= radius; dz++)
			for (int dx = -radius; dx <= radius; dx++)
			{
				if (radius > 0 && abs(dx) != radius && abs(dz) != radius) continue;

				int x = base_x + dx;
				int z = base_z + dz;
				int cx = world_chunk_coord((float)x);
				int cz = world_chunk_coord((float)z);

				int gx = cx - w->center_cx + WORLD_VIEW_RADIUS;
				int gz = cz - w->center_cz + WORLD_VIEW_RADIUS;
				if (gx < 0 || gz < 0 || gx >= WORLD_VIEW_CHUNKS || gz >= WORLD_VIEW_CHUNKS) continue;

				const World_Chunk* ch = w->grid[gz * WORLD_VIEW_CHUNKS + gx];
				if (!ch || !ch->used) continue;

				int lx = x - cx * WORLD_CHUNK;
				int lz = z - cz * WORLD_CHUNK;
				int h = ch->heights[lz * WORLD_CHUNK + lx];
				if (h < c.sea_level + 4.f || h > c.sea_level + 34.f) continue;

				// spawn on a walkable surface, not on a trunk or a leaf crown
				uint8 top = ch->blocks[world_chunk_block_index(lx, h - 1, lz)];
				if (top != WORLD_GRASS && top != WORLD_SAND && top != WORLD_SNOW && top != WORLD_STONE)
					continue;

				int score = 0;
				for (int nz = -1; nz <= 1; nz++)
					for (int nx = -1; nx <= 1; nx++)
					{
						if (nx == 0 && nz == 0) continue;
						float nh = world_height_at(c, (float)(x + nx) + .5f,
						                              (float)(z + nz) + .5f);
						int d = (int)(nh + .5f) - h;
						if (d < 0) d = -d;
						if (d > score) score = d;
					}

				if (score < best_score)
				{
					best_score = score;
					best = vec3((float)x + .5f, (float)h + .05f, (float)z + .5f);
				}
				if (score == 0)
				{
					*out = best;
					return true;
				}
			}
	}

	*out = best;
	return true;
}
