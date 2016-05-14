/*
Minetest
Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "map.h"
#include "mapsector.h"
#include "mapblock.h"
#include "filesys.h"
#include "voxel.h"
#include "porting.h"
#include "serialization.h"
#include "nodemetadata.h"
#include "settings.h"
#include "log.h"
#include "profiler.h"
#include "nodedef.h"
#include "gamedef.h"
#include "util/directiontables.h"
#include "util/mathconstants.h"
#include "rollback_interface.h"
#include "environment.h"
#include "emerge.h"
#include "mapgen_v6.h"
#include "mg_biome.h"
#include "config.h"
#include "server.h"
#include "database.h"
#include "database-dummy.h"
#include "database-sqlite3.h"
#include <deque>
#include <queue>
#if USE_LEVELDB
#include "database-leveldb.h"
#endif
#if USE_REDIS
#include "database-redis.h"
#endif

#define PP(x) "("<<(x).X<<","<<(x).Y<<","<<(x).Z<<")"


/*
	Map
*/

Map::Map(std::ostream &dout, IGameDef *gamedef):
	m_dout(dout),
	m_gamedef(gamedef),
	m_sector_cache(NULL),
	m_transforming_liquid_loop_count_multiplier(1.0f),
	m_unprocessed_count(0),
	m_inc_trending_up_start_time(0),
	m_queue_size_timer_started(false)
{
}

Map::~Map()
{
	/*
		Free all MapSectors
	*/
	for(std::map<v2s16, MapSector*>::iterator i = m_sectors.begin();
		i != m_sectors.end(); ++i)
	{
		delete i->second;
	}
}

void Map::addEventReceiver(MapEventReceiver *event_receiver)
{
	m_event_receivers.insert(event_receiver);
}

void Map::removeEventReceiver(MapEventReceiver *event_receiver)
{
	m_event_receivers.erase(event_receiver);
}

void Map::dispatchEvent(MapEditEvent *event)
{
	for(std::set<MapEventReceiver*>::iterator
			i = m_event_receivers.begin();
			i != m_event_receivers.end(); ++i)
	{
		(*i)->onMapEditEvent(event);
	}
}

MapSector * Map::getSectorNoGenerateNoExNoLock(v2s16 p)
{
	if(m_sector_cache != NULL && p == m_sector_cache_p){
		MapSector * sector = m_sector_cache;
		return sector;
	}

	std::map<v2s16, MapSector*>::iterator n = m_sectors.find(p);

	if(n == m_sectors.end())
		return NULL;

	MapSector *sector = n->second;

	// Cache the last result
	m_sector_cache_p = p;
	m_sector_cache = sector;

	return sector;
}

MapSector * Map::getSectorNoGenerateNoEx(v2s16 p)
{
	return getSectorNoGenerateNoExNoLock(p);
}

MapSector * Map::getSectorNoGenerate(v2s16 p)
{
	MapSector *sector = getSectorNoGenerateNoEx(p);
	if(sector == NULL)
		throw InvalidPositionException();

	return sector;
}

MapBlock * Map::getBlockNoCreateNoEx(v3s16 p3d)
{
	v2s16 p2d(p3d.X, p3d.Z);
	MapSector * sector = getSectorNoGenerateNoEx(p2d);
	if(sector == NULL)
		return NULL;
	MapBlock *block = sector->getBlockNoCreateNoEx(p3d.Y);
	return block;
}

MapBlock * Map::getBlockNoCreate(v3s16 p3d)
{
	MapBlock *block = getBlockNoCreateNoEx(p3d);
	if(block == NULL)
		throw InvalidPositionException();
	return block;
}

bool Map::isNodeUnderground(v3s16 p)
{
	v3s16 blockpos = getNodeBlockPos(p);
	try{
		MapBlock * block = getBlockNoCreate(blockpos);
		return block->getIsUnderground();
	}
	catch(InvalidPositionException &e)
	{
		return false;
	}
}

bool Map::isValidPosition(v3s16 p)
{
	v3s16 blockpos = getNodeBlockPos(p);
	MapBlock *block = getBlockNoCreate(blockpos);
	return (block != NULL);
}

// Returns a CONTENT_IGNORE node if not found
MapNode Map::getNodeNoEx(v3s16 p, bool *is_valid_position)
{
	v3s16 blockpos = getNodeBlockPos(p);
	MapBlock *block = getBlockNoCreateNoEx(blockpos);
	if (block == NULL) {
		if (is_valid_position != NULL)
			*is_valid_position = false;
		return MapNode(CONTENT_IGNORE);
	}

	v3s16 relpos = p - blockpos*MAP_BLOCKSIZE;
	bool is_valid_p;
	MapNode node = block->getNodeNoCheck(relpos, &is_valid_p);
	if (is_valid_position != NULL)
		*is_valid_position = is_valid_p;
	return node;
}

#if 0
// Deprecated
// throws InvalidPositionException if not found
// TODO: Now this is deprecated, getNodeNoEx should be renamed
MapNode Map::getNode(v3s16 p)
{
	v3s16 blockpos = getNodeBlockPos(p);
	MapBlock *block = getBlockNoCreateNoEx(blockpos);
	if (block == NULL)
		throw InvalidPositionException();
	v3s16 relpos = p - blockpos*MAP_BLOCKSIZE;
	bool is_valid_position;
	MapNode node = block->getNodeNoCheck(relpos, &is_valid_position);
	if (!is_valid_position)
		throw InvalidPositionException();
	return node;
}
#endif

// throws InvalidPositionException if not found
void Map::setNode(v3s16 p, MapNode & n)
{
	v3s16 blockpos = getNodeBlockPos(p);
	MapBlock *block = getBlockNoCreate(blockpos);
	v3s16 relpos = p - blockpos*MAP_BLOCKSIZE;
	// Never allow placing CONTENT_IGNORE, it fucks up stuff
	if(n.getContent() == CONTENT_IGNORE){
		bool temp_bool;
		errorstream<<"Map::setNode(): Not allowing to place CONTENT_IGNORE"
				<<" while trying to replace \""
				<<m_gamedef->ndef()->get(block->getNodeNoCheck(relpos, &temp_bool)).name
				<<"\" at "<<PP(p)<<" (block "<<PP(blockpos)<<")"<<std::endl;
		debug_stacks_print_to(infostream);
		return;
	}
	block->setNodeNoCheck(relpos, n);
}


/*
	Goes recursively through the neighbours of the node.

	Alters only transparent nodes.

	If the lighting of the neighbour is lower than the lighting of
	the node was (before changing it to 0 at the step before), the
	lighting of the neighbour is set to 0 and then the same stuff
	repeats for the neighbour.

	The ending nodes of the routine are stored in light_sources.
	This is useful when a light is removed. In such case, this
	routine can be called for the light node and then again for
	light_sources to re-light the area without the removed light.

	values of from_nodes are lighting values.
*/
void Map::unspreadLight(enum LightBank bank,
		std::map<v3s16, u8> & from_nodes,
		std::set<v3s16> & light_sources,
		std::map<v3s16, MapBlock*>  & modified_blocks)
{
	INodeDefManager *nodemgr = m_gamedef->ndef();

	v3s16 dirs[6] = {
		v3s16(0,0,1), // back
		v3s16(0,1,0), // top
		v3s16(1,0,0), // right
		v3s16(0,0,-1), // front
		v3s16(0,-1,0), // bottom
		v3s16(-1,0,0), // left
	};

	if(from_nodes.empty())
		return;

	u32 blockchangecount = 0;

	std::map<v3s16, u8> unlighted_nodes;

	/*
		Initialize block cache
	*/
	v3s16 blockpos_last;
	MapBlock *block = NULL;
	// Cache this a bit, too
	bool block_checked_in_modified = false;

	for(std::map<v3s16, u8>::iterator j = from_nodes.begin();
		j != from_nodes.end(); ++j)
	{
		v3s16 pos = j->first;
		v3s16 blockpos = getNodeBlockPos(pos);

		// Only fetch a new block if the block position has changed
		try{
			if(block == NULL || blockpos != blockpos_last){
				block = getBlockNoCreate(blockpos);
				blockpos_last = blockpos;

				block_checked_in_modified = false;
				blockchangecount++;
			}
		}
		catch(InvalidPositionException &e)
		{
			continue;
		}

		if(block->isDummy())
			continue;

		// Calculate relative position in block
		//v3s16 relpos = pos - blockpos_last * MAP_BLOCKSIZE;

		// Get node straight from the block
		//MapNode n = block->getNode(relpos);

		u8 oldlight = j->second;

		// Loop through 6 neighbors
		for(u16 i=0; i<6; i++)
		{
			// Get the position of the neighbor node
			v3s16 n2pos = pos + dirs[i];

			// Get the block where the node is located
			v3s16 blockpos, relpos;
			getNodeBlockPosWithOffset(n2pos, blockpos, relpos);

			// Only fetch a new block if the block position has changed
			try {
				if(block == NULL || blockpos != blockpos_last){
					block = getBlockNoCreate(blockpos);
					blockpos_last = blockpos;

					block_checked_in_modified = false;
					blockchangecount++;
				}
			}
			catch(InvalidPositionException &e) {
				continue;
			}

			// Get node straight from the block
			bool is_valid_position;
			MapNode n2 = block->getNode(relpos, &is_valid_position);
			if (!is_valid_position)
				continue;

			bool changed = false;

			//TODO: Optimize output by optimizing light_sources?

			/*
				If the neighbor is dimmer than what was specified
				as oldlight (the light of the previous node)
			*/
			if(n2.getLight(bank, nodemgr) < oldlight)
			{
				/*
					And the neighbor is transparent and it has some light
				*/
				if(nodemgr->get(n2).light_propagates
						&& n2.getLight(bank, nodemgr) != 0)
				{
					/*
						Set light to 0 and add to queue
					*/

					u8 current_light = n2.getLight(bank, nodemgr);
					n2.setLight(bank, 0, nodemgr);
					block->setNode(relpos, n2);

					unlighted_nodes[n2pos] = current_light;
					changed = true;

					/*
						Remove from light_sources if it is there
						NOTE: This doesn't happen nearly at all
					*/
					/*if(light_sources.find(n2pos))
					{
						infostream<<"Removed from light_sources"<<std::endl;
						light_sources.remove(n2pos);
					}*/
				}

				/*// DEBUG
				if(light_sources.find(n2pos) != NULL)
					light_sources.remove(n2pos);*/
			}
			else{
				light_sources.insert(n2pos);
			}

			// Add to modified_blocks
			if(changed == true && block_checked_in_modified == false)
			{
				// If the block is not found in modified_blocks, add.
				if(modified_blocks.find(blockpos) == modified_blocks.end())
				{
					modified_blocks[blockpos] = block;
				}
				block_checked_in_modified = true;
			}
		}
	}

	/*infostream<<"unspreadLight(): Changed block "
	<<blockchangecount<<" times"
	<<" for "<<from_nodes.size()<<" nodes"
	<<std::endl;*/

	if(!unlighted_nodes.empty())
		unspreadLight(bank, unlighted_nodes, light_sources, modified_blocks);
}

/*
	A single-node wrapper of the above
*/
void Map::unLightNeighbors(enum LightBank bank,
		v3s16 pos, u8 lightwas,
		std::set<v3s16> & light_sources,
		std::map<v3s16, MapBlock*>  & modified_blocks)
{
	std::map<v3s16, u8> from_nodes;
	from_nodes[pos] = lightwas;

	unspreadLight(bank, from_nodes, light_sources, modified_blocks);
}

/*
	Lights neighbors of from_nodes, collects all them and then
	goes on recursively.
*/
void Map::spreadLight(enum LightBank bank,
		std::set<v3s16> & from_nodes,
		std::map<v3s16, MapBlock*> & modified_blocks)
{
	INodeDefManager *nodemgr = m_gamedef->ndef();

	const v3s16 dirs[6] = {
		v3s16(0,0,1), // back
		v3s16(0,1,0), // top
		v3s16(1,0,0), // right
		v3s16(0,0,-1), // front
		v3s16(0,-1,0), // bottom
		v3s16(-1,0,0), // left
	};

	if(from_nodes.empty())
		return;

	u32 blockchangecount = 0;

	std::set<v3s16> lighted_nodes;

	/*
		Initialize block cache
	*/
	v3s16 blockpos_last;
	MapBlock *block = NULL;
		// Cache this a bit, too
	bool block_checked_in_modified = false;

	for(std::set<v3s16>::iterator j = from_nodes.begin();
		j != from_nodes.end(); ++j)
	{
		v3s16 pos = *j;
		v3s16 blockpos, relpos;

		getNodeBlockPosWithOffset(pos, blockpos, relpos);

		// Only fetch a new block if the block position has changed
		try {
			if(block == NULL || blockpos != blockpos_last){
				block = getBlockNoCreate(blockpos);
				blockpos_last = blockpos;

				block_checked_in_modified = false;
				blockchangecount++;
			}
		}
		catch(InvalidPositionException &e) {
			continue;
		}

		if(block->isDummy())
			continue;

		// Get node straight from the block
		bool is_valid_position;
		MapNode n = block->getNode(relpos, &is_valid_position);

		u8 oldlight = is_valid_position ? n.getLight(bank, nodemgr) : 0;
		u8 newlight = diminish_light(oldlight);

		// Loop through 6 neighbors
		for(u16 i=0; i<6; i++){
			// Get the position of the neighbor node
			v3s16 n2pos = pos + dirs[i];

			// Get the block where the node is located
			v3s16 blockpos, relpos;
			getNodeBlockPosWithOffset(n2pos, blockpos, relpos);

			// Only fetch a new block if the block position has changed
			try {
				if(block == NULL || blockpos != blockpos_last){
					block = getBlockNoCreate(blockpos);
					blockpos_last = blockpos;

					block_checked_in_modified = false;
					blockchangecount++;
				}
			}
			catch(InvalidPositionException &e) {
				continue;
			}

			// Get node straight from the block
			MapNode n2 = block->getNode(relpos, &is_valid_position);
			if (!is_valid_position)
				continue;

			bool changed = false;
			/*
				If the neighbor is brighter than the current node,
				add to list (it will light up this node on its turn)
			*/
			if(n2.getLight(bank, nodemgr) > undiminish_light(oldlight))
			{
				lighted_nodes.insert(n2pos);
				changed = true;
			}
			/*
				If the neighbor is dimmer than how much light this node
				would spread on it, add to list
			*/
			if(n2.getLight(bank, nodemgr) < newlight)
			{
				if(nodemgr->get(n2).light_propagates)
				{
					n2.setLight(bank, newlight, nodemgr);
					block->setNode(relpos, n2);
					lighted_nodes.insert(n2pos);
					changed = true;
				}
			}

			// Add to modified_blocks
			if(changed == true && block_checked_in_modified == false)
			{
				// If the block is not found in modified_blocks, add.
				if(modified_blocks.find(blockpos) == modified_blocks.end())
				{
					modified_blocks[blockpos] = block;
				}
				block_checked_in_modified = true;
			}
		}
	}

	/*infostream<<"spreadLight(): Changed block "
			<<blockchangecount<<" times"
			<<" for "<<from_nodes.size()<<" nodes"
			<<std::endl;*/

	if(!lighted_nodes.empty())
		spreadLight(bank, lighted_nodes, modified_blocks);
}

/*
	A single-node source variation of the above.
*/
void Map::lightNeighbors(enum LightBank bank,
		v3s16 pos,
		std::map<v3s16, MapBlock*> & modified_blocks)
{
	std::set<v3s16> from_nodes;
	from_nodes.insert(pos);
	spreadLight(bank, from_nodes, modified_blocks);
}

v3s16 Map::getBrightestNeighbour(enum LightBank bank, v3s16 p)
{
	INodeDefManager *nodemgr = m_gamedef->ndef();

	v3s16 dirs[6] = {
		v3s16(0,0,1), // back
		v3s16(0,1,0), // top
		v3s16(1,0,0), // right
		v3s16(0,0,-1), // front
		v3s16(0,-1,0), // bottom
		v3s16(-1,0,0), // left
	};

	u8 brightest_light = 0;
	v3s16 brightest_pos(0,0,0);
	bool found_something = false;

	// Loop through 6 neighbors
	for(u16 i=0; i<6; i++){
		// Get the position of the neighbor node
		v3s16 n2pos = p + dirs[i];
		MapNode n2;
		bool is_valid_position;
		n2 = getNodeNoEx(n2pos, &is_valid_position);
		if (!is_valid_position)
			continue;

		if(n2.getLight(bank, nodemgr) > brightest_light || found_something == false){
			brightest_light = n2.getLight(bank, nodemgr);
			brightest_pos = n2pos;
			found_something = true;
		}
	}

	if(found_something == false)
		throw InvalidPositionException();

	return brightest_pos;
}

/*
	Propagates sunlight down from a node.
	Starting point gets sunlight.

	Returns the lowest y value of where the sunlight went.

	Mud is turned into grass in where the sunlight stops.
*/
s16 Map::propagateSunlight(v3s16 start,
		std::map<v3s16, MapBlock*> & modified_blocks)
{
	INodeDefManager *nodemgr = m_gamedef->ndef();

	s16 y = start.Y;
	for(; ; y--)
	{
		v3s16 pos(start.X, y, start.Z);

		v3s16 blockpos = getNodeBlockPos(pos);
		MapBlock *block;
		try{
			block = getBlockNoCreate(blockpos);
		}
		catch(InvalidPositionException &e)
		{
			break;
		}

		v3s16 relpos = pos - blockpos*MAP_BLOCKSIZE;
		bool is_valid_position;
		MapNode n = block->getNode(relpos, &is_valid_position);
		if (!is_valid_position)
			break;

		if(nodemgr->get(n).sunlight_propagates)
		{
			n.setLight(LIGHTBANK_DAY, LIGHT_SUN, nodemgr);
			block->setNode(relpos, n);

			modified_blocks[blockpos] = block;
		}
		else
		{
			// Sunlight goes no further
			break;
		}
	}
	return y + 1;
}

void Map::updateLighting(enum LightBank bank,
		std::map<v3s16, MapBlock*> & a_blocks,
		std::map<v3s16, MapBlock*> & modified_blocks)
{
	INodeDefManager *nodemgr = m_gamedef->ndef();

	/*m_dout<<"Map::updateLighting(): "
			<<a_blocks.size()<<" blocks."<<std::endl;*/

	//TimeTaker timer("updateLighting");

	// For debugging
	//bool debug=true;
	//u32 count_was = modified_blocks.size();

	//std::map<v3s16, MapBlock*> blocks_to_update;

	std::set<v3s16> light_sources;

	std::map<v3s16, u8> unlight_from;

	int num_bottom_invalid = 0;

	{
	//TimeTaker t("first stuff");

	for(std::map<v3s16, MapBlock*>::iterator i = a_blocks.begin();
		i != a_blocks.end(); ++i)
	{
		MapBlock *block = i->second;

		for(;;)
		{
			// Don't bother with dummy blocks.
			if(block->isDummy())
				break;

			v3s16 pos = block->getPos();
			v3s16 posnodes = block->getPosRelative();
			modified_blocks[pos] = block;
			//blocks_to_update[pos] = block;

			/*
				Clear all light from block
			*/
			for(s16 z=0; z<MAP_BLOCKSIZE; z++)
			for(s16 x=0; x<MAP_BLOCKSIZE; x++)
			for(s16 y=0; y<MAP_BLOCKSIZE; y++)
			{
				v3s16 p(x,y,z);
				bool is_valid_position;
				MapNode n = block->getNode(p, &is_valid_position);
				if (!is_valid_position) {
					/* This would happen when dealing with a
					   dummy block.
					*/
					infostream<<"updateLighting(): InvalidPositionException"
							<<std::endl;
					continue;
				}
				u8 oldlight = n.getLight(bank, nodemgr);
				n.setLight(bank, 0, nodemgr);
				block->setNode(p, n);

				// If node sources light, add to list
				u8 source = nodemgr->get(n).light_source;
				if(source != 0)
					light_sources.insert(p + posnodes);

				// Collect borders for unlighting
				if((x==0 || x == MAP_BLOCKSIZE-1
						|| y==0 || y == MAP_BLOCKSIZE-1
						|| z==0 || z == MAP_BLOCKSIZE-1)
						&& oldlight != 0)
				{
					v3s16 p_map = p + posnodes;
					unlight_from[p_map] = oldlight;
				}


			}

			if(bank == LIGHTBANK_DAY)
			{
				bool bottom_valid = block->propagateSunlight(light_sources);

				if(!bottom_valid)
					num_bottom_invalid++;

				// If bottom is valid, we're done.
				if(bottom_valid)
					break;
			}
			else if(bank == LIGHTBANK_NIGHT)
			{
				// For night lighting, sunlight is not propagated
				break;
			}
			else
			{
				assert("Invalid lighting bank" == NULL);
			}

			/*infostream<<"Bottom for sunlight-propagated block ("
					<<pos.X<<","<<pos.Y<<","<<pos.Z<<") not valid"
					<<std::endl;*/

			// Bottom sunlight is not valid; get the block and loop to it

			pos.Y--;
			try{
				block = getBlockNoCreate(pos);
			}
			catch(InvalidPositionException &e)
			{
				FATAL_ERROR("Invalid position");
			}

		}
	}

	}

	/*
		Enable this to disable proper lighting for speeding up map
		generation for testing or whatever
	*/
#if 0
	//if(g_settings->get(""))
	{
		core::map<v3s16, MapBlock*>::Iterator i;
		i = blocks_to_update.getIterator();
		for(; i.atEnd() == false; i++)
		{
			MapBlock *block = i.getNode()->getValue();
			v3s16 p = block->getPos();
			block->setLightingExpired(false);
		}
		return;
	}
#endif

#if 1
	{
		//TimeTaker timer("unspreadLight");
		unspreadLight(bank, unlight_from, light_sources, modified_blocks);
	}

	/*if(debug)
	{
		u32 diff = modified_blocks.size() - count_was;
		count_was = modified_blocks.size();
		infostream<<"unspreadLight modified "<<diff<<std::endl;
	}*/

	{
		//TimeTaker timer("spreadLight");
		spreadLight(bank, light_sources, modified_blocks);
	}

	/*if(debug)
	{
		u32 diff = modified_blocks.size() - count_was;
		count_was = modified_blocks.size();
		infostream<<"spreadLight modified "<<diff<<std::endl;
	}*/
#endif

#if 0
	{
		//MapVoxelManipulator vmanip(this);

		// Make a manual voxel manipulator and load all the blocks
		// that touch the requested blocks
		ManualMapVoxelManipulator vmanip(this);

		{
		//TimeTaker timer("initialEmerge");

		core::map<v3s16, MapBlock*>::Iterator i;
		i = blocks_to_update.getIterator();
		for(; i.atEnd() == false; i++)
		{
			MapBlock *block = i.getNode()->getValue();
			v3s16 p = block->getPos();

			// Add all surrounding blocks
			vmanip.initialEmerge(p - v3s16(1,1,1), p + v3s16(1,1,1));

			/*
				Add all surrounding blocks that have up-to-date lighting
				NOTE: This doesn't quite do the job (not everything
					  appropriate is lighted)
			*/
			/*for(s16 z=-1; z<=1; z++)
			for(s16 y=-1; y<=1; y++)
			for(s16 x=-1; x<=1; x++)
			{
				v3s16 p2 = p + v3s16(x,y,z);
				MapBlock *block = getBlockNoCreateNoEx(p2);
				if(block == NULL)
					continue;
				if(block->isDummy())
					continue;
				if(block->getLightingExpired())
					continue;
				vmanip.initialEmerge(p2, p2);
			}*/

			// Lighting of block will be updated completely
			block->setLightingExpired(false);
		}
		}

		{
			//TimeTaker timer("unSpreadLight");
			vmanip.unspreadLight(bank, unlight_from, light_sources, nodemgr);
		}
		{
			//TimeTaker timer("spreadLight");
			vmanip.spreadLight(bank, light_sources, nodemgr);
		}
		{
			//TimeTaker timer("blitBack");
			vmanip.blitBack(modified_blocks);
		}
		/*infostream<<"emerge_time="<<emerge_time<<std::endl;
		emerge_time = 0;*/
	}
#endif

	//m_dout<<"Done ("<<getTimestamp()<<")"<<std::endl;
}

void Map::updateLighting(std::map<v3s16, MapBlock*> & a_blocks,
		std::map<v3s16, MapBlock*> & modified_blocks)
{
	updateLighting(LIGHTBANK_DAY, a_blocks, modified_blocks);
	updateLighting(LIGHTBANK_NIGHT, a_blocks, modified_blocks);

	/*
		Update information about whether day and night light differ
	*/
	for(std::map<v3s16, MapBlock*>::iterator
			i = modified_blocks.begin();
			i != modified_blocks.end(); ++i)
	{
		MapBlock *block = i->second;
		block->expireDayNightDiff();
	}
}

/*
*/
void Map::addNodeAndUpdate(v3s16 p, MapNode n,
		std::map<v3s16, MapBlock*> &modified_blocks,
		bool remove_metadata)
{
	INodeDefManager *ndef = m_gamedef->ndef();

	/*PrintInfo(m_dout);
	m_dout<<"Map::addNodeAndUpdate(): p=("
			<<p.X<<","<<p.Y<<","<<p.Z<<")"<<std::endl;*/

	/*
		From this node to nodes underneath:
		If lighting is sunlight (1.0), unlight neighbours and
		set lighting to 0.
		Else discontinue.
	*/

	v3s16 toppos = p + v3s16(0,1,0);
	//v3s16 bottompos = p + v3s16(0,-1,0);

	bool node_under_sunlight = true;
	std::set<v3s16> light_sources;

	/*
		Collect old node for rollback
	*/
	RollbackNode rollback_oldnode(this, p, m_gamedef);

	/*
		If there is a node at top and it doesn't have sunlight,
		there has not been any sunlight going down.

		Otherwise there probably is.
	*/

	bool is_valid_position;
	MapNode topnode = getNodeNoEx(toppos, &is_valid_position);

	if(is_valid_position && topnode.getLight(LIGHTBANK_DAY, ndef) != LIGHT_SUN)
		node_under_sunlight = false;

	/*
		Remove all light that has come out of this node
	*/

	enum LightBank banks[] =
	{
		LIGHTBANK_DAY,
		LIGHTBANK_NIGHT
	};
	for(s32 i=0; i<2; i++)
	{
		enum LightBank bank = banks[i];

		u8 lightwas = getNodeNoEx(p).getLight(bank, ndef);

		// Add the block of the added node to modified_blocks
		v3s16 blockpos = getNodeBlockPos(p);
		MapBlock * block = getBlockNoCreate(blockpos);
		assert(block != NULL);
		modified_blocks[blockpos] = block;

		assert(isValidPosition(p));

		// Unlight neighbours of node.
		// This means setting light of all consequent dimmer nodes
		// to 0.
		// This also collects the nodes at the border which will spread
		// light again into this.
		unLightNeighbors(bank, p, lightwas, light_sources, modified_blocks);

		n.setLight(bank, 0, ndef);
	}

	/*
		If node lets sunlight through and is under sunlight, it has
		sunlight too.
	*/
	if(node_under_sunlight && ndef->get(n).sunlight_propagates)
	{
		n.setLight(LIGHTBANK_DAY, LIGHT_SUN, ndef);
	}

	/*
		Remove node metadata
	*/
	if (remove_metadata) {
		removeNodeMetadata(p);
	}

	/*
		Set the node on the map
	*/

	setNode(p, n);

	/*
		If node is under sunlight and doesn't let sunlight through,
		take all sunlighted nodes under it and clear light from them
		and from where the light has been spread.
		TODO: This could be optimized by mass-unlighting instead
			  of looping
	*/
	if(node_under_sunlight && !ndef->get(n).sunlight_propagates)
	{
		s16 y = p.Y - 1;
		for(;; y--){
			//m_dout<<"y="<<y<<std::endl;
			v3s16 n2pos(p.X, y, p.Z);

			MapNode n2;

			n2 = getNodeNoEx(n2pos, &is_valid_position);
			if (!is_valid_position)
				break;

			if(n2.getLight(LIGHTBANK_DAY, ndef) == LIGHT_SUN)
			{
				unLightNeighbors(LIGHTBANK_DAY,
						n2pos, n2.getLight(LIGHTBANK_DAY, ndef),
						light_sources, modified_blocks);
				n2.setLight(LIGHTBANK_DAY, 0, ndef);
				setNode(n2pos, n2);
			}
			else
				break;
		}
	}

	for(s32 i=0; i<2; i++)
	{
		enum LightBank bank = banks[i];

		/*
			Spread light from all nodes that might be capable of doing so
		*/
		spreadLight(bank, light_sources, modified_blocks);
	}

	/*
		Update information about whether day and night light differ
	*/
	for(std::map<v3s16, MapBlock*>::iterator
			i = modified_blocks.begin();
			i != modified_blocks.end(); ++i)
	{
		i->second->expireDayNightDiff();
	}

	/*
		Report for rollback
	*/
	if(m_gamedef->rollback())
	{
		RollbackNode rollback_newnode(this, p, m_gamedef);
		RollbackAction action;
		action.setSetNode(p, rollback_oldnode, rollback_newnode);
		m_gamedef->rollback()->reportAction(action);
	}

	/*
		Add neighboring liquid nodes and the node itself if it is
		liquid (=water node was added) to transform queue.
	*/
	v3s16 dirs[7] = {
		v3s16(0,0,0), // self
		v3s16(0,0,1), // back
		v3s16(0,1,0), // top
		v3s16(1,0,0), // right
		v3s16(0,0,-1), // front
		v3s16(0,-1,0), // bottom
		v3s16(-1,0,0), // left
	};
	for(u16 i=0; i<7; i++)
	{
		v3s16 p2 = p + dirs[i];

		MapNode n2 = getNodeNoEx(p2, &is_valid_position);
		if(is_valid_position
				&& (ndef->get(n2).isLiquid() || n2.getContent() == CONTENT_AIR))
		{
			m_transforming_liquid.push_back(p2);
		}
	}
}

/*
*/
void Map::removeNodeAndUpdate(v3s16 p,
		std::map<v3s16, MapBlock*> &modified_blocks)
{
	INodeDefManager *ndef = m_gamedef->ndef();

	/*PrintInfo(m_dout);
	m_dout<<"Map::removeNodeAndUpdate(): p=("
			<<p.X<<","<<p.Y<<","<<p.Z<<")"<<std::endl;*/

	bool node_under_sunlight = true;

	v3s16 toppos = p + v3s16(0,1,0);

	// Node will be replaced with this
	content_t replace_material = CONTENT_AIR;

	/*
		Collect old node for rollback
	*/
	RollbackNode rollback_oldnode(this, p, m_gamedef);

	/*
		If there is a node at top and it doesn't have sunlight,
		there will be no sunlight going down.
	*/
	bool is_valid_position;
	MapNode topnode = getNodeNoEx(toppos, &is_valid_position);

	if(is_valid_position && topnode.getLight(LIGHTBANK_DAY, ndef) != LIGHT_SUN)
		node_under_sunlight = false;

	std::set<v3s16> light_sources;

	enum LightBank banks[] =
	{
		LIGHTBANK_DAY,
		LIGHTBANK_NIGHT
	};
	for(s32 i=0; i<2; i++)
	{
		enum LightBank bank = banks[i];

		/*
			Unlight neighbors (in case the node is a light source)
		*/
		unLightNeighbors(bank, p,
				getNodeNoEx(p).getLight(bank, ndef),
				light_sources, modified_blocks);
	}

	/*
		Remove node metadata
	*/

	removeNodeMetadata(p);

	/*
		Remove the node.
		This also clears the lighting.
	*/

	MapNode n(replace_material);
	setNode(p, n);

	for(s32 i=0; i<2; i++)
	{
		enum LightBank bank = banks[i];

		/*
			Recalculate lighting
		*/
		spreadLight(bank, light_sources, modified_blocks);
	}

	// Add the block of the removed node to modified_blocks
	v3s16 blockpos = getNodeBlockPos(p);
	MapBlock * block = getBlockNoCreate(blockpos);
	assert(block != NULL);
	modified_blocks[blockpos] = block;

	/*
		If the removed node was under sunlight, propagate the
		sunlight down from it and then light all neighbors
		of the propagated blocks.
	*/
	if(node_under_sunlight)
	{
		s16 ybottom = propagateSunlight(p, modified_blocks);
		/*m_dout<<"Node was under sunlight. "
				"Propagating sunlight";
		m_dout<<" -> ybottom="<<ybottom<<std::endl;*/
		s16 y = p.Y;
		for(; y >= ybottom; y--)
		{
			v3s16 p2(p.X, y, p.Z);
			/*m_dout<<"lighting neighbors of node ("
					<<p2.X<<","<<p2.Y<<","<<p2.Z<<")"
					<<std::endl;*/
			lightNeighbors(LIGHTBANK_DAY, p2, modified_blocks);
		}
	}
	else
	{
		// Set the lighting of this node to 0
		// TODO: Is this needed? Lighting is cleared up there already.
		MapNode n = getNodeNoEx(p, &is_valid_position);
		if (is_valid_position) {
			n.setLight(LIGHTBANK_DAY, 0, ndef);
			setNode(p, n);
		} else {
			FATAL_ERROR("Invalid position");
		}
	}

	for(s32 i=0; i<2; i++)
	{
		enum LightBank bank = banks[i];

		// Get the brightest neighbour node and propagate light from it
		v3s16 n2p = getBrightestNeighbour(bank, p);
		try{
			//MapNode n2 = getNode(n2p);
			lightNeighbors(bank, n2p, modified_blocks);
		}
		catch(InvalidPositionException &e)
		{
		}
	}

	/*
		Update information about whether day and night light differ
	*/
	for(std::map<v3s16, MapBlock*>::iterator
			i = modified_blocks.begin();
			i != modified_blocks.end(); ++i)
	{
		i->second->expireDayNightDiff();
	}

	/*
		Report for rollback
	*/
	if(m_gamedef->rollback())
	{
		RollbackNode rollback_newnode(this, p, m_gamedef);
		RollbackAction action;
		action.setSetNode(p, rollback_oldnode, rollback_newnode);
		m_gamedef->rollback()->reportAction(action);
	}

	/*
		Add neighboring liquid nodes and this node to transform queue.
		(it's vital for the node itself to get updated last.)
	*/
	v3s16 dirs[7] = {
		v3s16(0,0,1), // back
		v3s16(0,1,0), // top
		v3s16(1,0,0), // right
		v3s16(0,0,-1), // front
		v3s16(0,-1,0), // bottom
		v3s16(-1,0,0), // left
		v3s16(0,0,0), // self
	};
	for(u16 i=0; i<7; i++)
	{
		v3s16 p2 = p + dirs[i];

		bool is_position_valid;
		MapNode n2 = getNodeNoEx(p2, &is_position_valid);
		if (is_position_valid
				&& (ndef->get(n2).isLiquid() || n2.getContent() == CONTENT_AIR))
		{
			m_transforming_liquid.push_back(p2);
		}
	}
}

bool Map::addNodeWithEvent(v3s16 p, MapNode n, bool remove_metadata)
{
	MapEditEvent event;
	event.type = remove_metadata ? MEET_ADDNODE : MEET_SWAPNODE;
	event.p = p;
	event.n = n;

	bool succeeded = true;
	try{
		std::map<v3s16, MapBlock*> modified_blocks;
		addNodeAndUpdate(p, n, modified_blocks, remove_metadata);

		// Copy modified_blocks to event
		for(std::map<v3s16, MapBlock*>::iterator
				i = modified_blocks.begin();
				i != modified_blocks.end(); ++i)
		{
			event.modified_blocks.insert(i->first);
		}
	}
	catch(InvalidPositionException &e){
		succeeded = false;
	}

	dispatchEvent(&event);

	return succeeded;
}

bool Map::removeNodeWithEvent(v3s16 p)
{
	MapEditEvent event;
	event.type = MEET_REMOVENODE;
	event.p = p;

	bool succeeded = true;
	try{
		std::map<v3s16, MapBlock*> modified_blocks;
		removeNodeAndUpdate(p, modified_blocks);

		// Copy modified_blocks to event
		for(std::map<v3s16, MapBlock*>::iterator
				i = modified_blocks.begin();
				i != modified_blocks.end(); ++i)
		{
			event.modified_blocks.insert(i->first);
		}
	}
	catch(InvalidPositionException &e){
		succeeded = false;
	}

	dispatchEvent(&event);

	return succeeded;
}

bool Map::getDayNightDiff(v3s16 blockpos)
{
	try{
		v3s16 p = blockpos + v3s16(0,0,0);
		MapBlock *b = getBlockNoCreate(p);
		if(b->getDayNightDiff())
			return true;
	}
	catch(InvalidPositionException &e){}
	// Leading edges
	try{
		v3s16 p = blockpos + v3s16(-1,0,0);
		MapBlock *b = getBlockNoCreate(p);
		if(b->getDayNightDiff())
			return true;
	}
	catch(InvalidPositionException &e){}
	try{
		v3s16 p = blockpos + v3s16(0,-1,0);
		MapBlock *b = getBlockNoCreate(p);
		if(b->getDayNightDiff())
			return true;
	}
	catch(InvalidPositionException &e){}
	try{
		v3s16 p = blockpos + v3s16(0,0,-1);
		MapBlock *b = getBlockNoCreate(p);
		if(b->getDayNightDiff())
			return true;
	}
	catch(InvalidPositionException &e){}
	// Trailing edges
	try{
		v3s16 p = blockpos + v3s16(1,0,0);
		MapBlock *b = getBlockNoCreate(p);
		if(b->getDayNightDiff())
			return true;
	}
	catch(InvalidPositionException &e){}
	try{
		v3s16 p = blockpos + v3s16(0,1,0);
		MapBlock *b = getBlockNoCreate(p);
		if(b->getDayNightDiff())
			return true;
	}
	catch(InvalidPositionException &e){}
	try{
		v3s16 p = blockpos + v3s16(0,0,1);
		MapBlock *b = getBlockNoCreate(p);
		if(b->getDayNightDiff())
			return true;
	}
	catch(InvalidPositionException &e){}

	return false;
}

struct TimeOrderedMapBlock {
	MapSector *sect;
	MapBlock *block;

	TimeOrderedMapBlock(MapSector *sect, MapBlock *block) :
		sect(sect),
		block(block)
	{}

	bool operator<(const TimeOrderedMapBlock &b) const
	{
		return block->getUsageTimer() < b.block->getUsageTimer();
	};
};

/*
	Updates usage timers
*/
void Map::timerUpdate(float dtime, float unload_timeout, u32 max_loaded_blocks,
		std::vector<v3s16> *unloaded_blocks)
{
	bool save_before_unloading = (mapType() == MAPTYPE_SERVER);

	// Profile modified reasons
	Profiler modprofiler;

	std::vector<v2s16> sector_deletion_queue;
	u32 deleted_blocks_count = 0;
	u32 saved_blocks_count = 0;
	u32 block_count_all = 0;

	beginSave();

	// If there is no practical limit, we spare creation of mapblock_queue
	if (max_loaded_blocks == U32_MAX) {
		for (std::map<v2s16, MapSector*>::iterator si = m_sectors.begin();
				si != m_sectors.end(); ++si) {
			MapSector *sector = si->second;

			bool all_blocks_deleted = true;

			MapBlockVect blocks;
			sector->getBlocks(blocks);

			for (MapBlockVect::iterator i = blocks.begin();
					i != blocks.end(); ++i) {
				MapBlock *block = (*i);

				block->incrementUsageTimer(dtime);

				if (block->refGet() == 0
						&& block->getUsageTimer() > unload_timeout) {
					v3s16 p = block->getPos();

					// Save if modified
					if (block->getModified() != MOD_STATE_CLEAN
							&& save_before_unloading) {
						modprofiler.add(block->getModifiedReasonString(), 1);
						if (!saveBlock(block))
							continue;
						saved_blocks_count++;
					}

					// Delete from memory
					sector->deleteBlock(block);

					if (unloaded_blocks)
						unloaded_blocks->push_back(p);

					deleted_blocks_count++;
				} else {
					all_blocks_deleted = false;
					block_count_all++;
				}
			}

			if (all_blocks_deleted) {
				sector_deletion_queue.push_back(si->first);
			}
		}
	} else {
		std::priority_queue<TimeOrderedMapBlock> mapblock_queue;
		for (std::map<v2s16, MapSector*>::iterator si = m_sectors.begin();
				si != m_sectors.end(); ++si) {
			MapSector *sector = si->second;

			MapBlockVect blocks;
			sector->getBlocks(blocks);

			for(MapBlockVect::iterator i = blocks.begin();
					i != blocks.end(); ++i) {
				MapBlock *block = (*i);

				block->incrementUsageTimer(dtime);
				mapblock_queue.push(TimeOrderedMapBlock(sector, block));
			}
		}
		block_count_all = mapblock_queue.size();
		// Delete old blocks, and blocks over the limit from the memory
		while (!mapblock_queue.empty() && (mapblock_queue.size() > max_loaded_blocks
				|| mapblock_queue.top().block->getUsageTimer() > unload_timeout)) {
			TimeOrderedMapBlock b = mapblock_queue.top();
			mapblock_queue.pop();

			MapBlock *block = b.block;

			if (block->refGet() != 0)
				continue;

			v3s16 p = block->getPos();

			// Save if modified
			if (block->getModified() != MOD_STATE_CLEAN && save_before_unloading) {
				modprofiler.add(block->getModifiedReasonString(), 1);
				if (!saveBlock(block))
					continue;
				saved_blocks_count++;
			}

			// Delete from memory
			b.sect->deleteBlock(block);

			if (unloaded_blocks)
				unloaded_blocks->push_back(p);

			deleted_blocks_count++;
			block_count_all--;
		}
		// Delete empty sectors
		for (std::map<v2s16, MapSector*>::iterator si = m_sectors.begin();
			si != m_sectors.end(); ++si) {
			if (si->second->empty()) {
				sector_deletion_queue.push_back(si->first);
			}
		}
	}
	endSave();

	// Finally delete the empty sectors
	deleteSectors(sector_deletion_queue);

	if(deleted_blocks_count != 0)
	{
		PrintInfo(infostream); // ServerMap/ClientMap:
		infostream<<"Unloaded "<<deleted_blocks_count
				<<" blocks from memory";
		if(save_before_unloading)
			infostream<<", of which "<<saved_blocks_count<<" were written";
		infostream<<", "<<block_count_all<<" blocks in memory";
		infostream<<"."<<std::endl;
		if(saved_blocks_count != 0){
			PrintInfo(infostream); // ServerMap/ClientMap:
			infostream<<"Blocks modified by: "<<std::endl;
			modprofiler.print(infostream);
		}
	}
}

void Map::unloadUnreferencedBlocks(std::vector<v3s16> *unloaded_blocks)
{
	timerUpdate(0.0, -1.0, 0, unloaded_blocks);
}

void Map::deleteSectors(std::vector<v2s16> &sectorList)
{
	for(std::vector<v2s16>::iterator j = sectorList.begin();
		j != sectorList.end(); ++j) {
		MapSector *sector = m_sectors[*j];
		// If sector is in sector cache, remove it from there
		if(m_sector_cache == sector)
			m_sector_cache = NULL;
		// Remove from map and delete
		m_sectors.erase(*j);
		delete sector;
	}
}

void Map::PrintInfo(std::ostream &out)
{
	out<<"Map: ";
}

#define WATER_DROP_BOOST 4

enum NeighborType {
	NEIGHBOR_UPPER,
	NEIGHBOR_SAME_LEVEL,
	NEIGHBOR_LOWER
};
struct NodeNeighbor {
	MapNode n;
	NeighborType t;
	v3s16 p;
	bool l; //can liquid

	NodeNeighbor()
		: n(CONTENT_AIR)
	{ }

	NodeNeighbor(const MapNode &node, NeighborType n_type, v3s16 pos)
		: n(node),
		  t(n_type),
		  p(pos)
	{ }
};

void Map::transforming_liquid_add(v3s16 p) {
        m_transforming_liquid.push_back(p);
}

s32 Map::transforming_liquid_size() {
        return m_transforming_liquid.size();
}

void Map::transformLiquids(std::map<v3s16, MapBlock*> &modified_blocks)
{

	INodeDefManager *nodemgr = m_gamedef->ndef();

	DSTACK(FUNCTION_NAME);
	//TimeTaker timer("transformLiquids()");

	u32 loopcount = 0;
	u32 initial_size = m_transforming_liquid.size();

	/*if(initial_size != 0)
		infostream<<"transformLiquids(): initial_size="<<initial_size<<std::endl;*/

	// list of nodes that due to viscosity have not reached their max level height
	std::deque<v3s16> must_reflow;

	// List of MapBlocks that will require a lighting update (due to lava)
	std::map<v3s16, MapBlock *> lighting_modified_blocks;

	u32 liquid_loop_max = g_settings->getS32("liquid_loop_max");
	u32 loop_max = liquid_loop_max;

#if 0

	/* If liquid_loop_max is not keeping up with the queue size increase
	 * loop_max up to a maximum of liquid_loop_max * dedicated_server_step.
	 */
	if (m_transforming_liquid.size() > loop_max * 2) {
		// "Burst" mode
		float server_step = g_settings->getFloat("dedicated_server_step");
		if (m_transforming_liquid_loop_count_multiplier - 1.0 < server_step)
			m_transforming_liquid_loop_count_multiplier *= 1.0 + server_step / 10;
	} else {
		m_transforming_liquid_loop_count_multiplier = 1.0;
	}

	loop_max *= m_transforming_liquid_loop_count_multiplier;
#endif

	while (m_transforming_liquid.size() != 0)
	{
		// This should be done here so that it is done when continue is used
		if (loopcount >= initial_size || loopcount >= loop_max)
			break;
		loopcount++;

		/*
			Get a queued transforming liquid node
		*/
		v3s16 p0 = m_transforming_liquid.front();
		m_transforming_liquid.pop_front();

		MapNode n0 = getNodeNoEx(p0);

		/*
			Collect information about current node
		 */
		s8 liquid_level = -1;
		content_t liquid_kind = CONTENT_IGNORE;
		content_t floodable_node = CONTENT_AIR;
		const ContentFeatures &cf = nodemgr->get(n0);
		LiquidType liquid_type = cf.liquid_type;
		switch (liquid_type) {
			case LIQUID_SOURCE:
				liquid_level = LIQUID_LEVEL_SOURCE;
				liquid_kind = nodemgr->getId(cf.liquid_alternative_flowing);
				break;
			case LIQUID_FLOWING:
				liquid_level = (n0.param2 & LIQUID_LEVEL_MASK);
				liquid_kind = n0.getContent();
				break;
			case LIQUID_NONE:
				// if this node is 'floodable', it *could* be transformed
				// into a liquid, otherwise, continue with the next node.
				if (!cf.floodable)
					continue;
				floodable_node = n0.getContent();
				liquid_kind = CONTENT_AIR;
				break;
		}

		/*
			Collect information about the environment
		 */
		const v3s16 *dirs = g_6dirs;
		NodeNeighbor sources[6]; // surrounding sources
		int num_sources = 0;
		NodeNeighbor flows[6]; // surrounding flowing liquid nodes
		int num_flows = 0;
		NodeNeighbor airs[6]; // surrounding air
		int num_airs = 0;
		NodeNeighbor neutrals[6]; // nodes that are solid or another kind of liquid
		int num_neutrals = 0;
		bool flowing_down = false;
		for (u16 i = 0; i < 6; i++) {
			NeighborType nt = NEIGHBOR_SAME_LEVEL;
			switch (i) {
				case 1:
					nt = NEIGHBOR_UPPER;
					break;
				case 4:
					nt = NEIGHBOR_LOWER;
					break;
			}
			v3s16 npos = p0 + dirs[i];
			NodeNeighbor nb(getNodeNoEx(npos), nt, npos);
			const ContentFeatures &cfnb = nodemgr->get(nb.n);
			switch (nodemgr->get(nb.n.getContent()).liquid_type) {
				case LIQUID_NONE:
					if (cfnb.floodable) {
						airs[num_airs++] = nb;
						// if the current node is a water source the neighbor
						// should be enqueded for transformation regardless of whether the
						// current node changes or not.
						if (nb.t != NEIGHBOR_UPPER && liquid_type != LIQUID_NONE)
							m_transforming_liquid.push_back(npos);
						// if the current node happens to be a flowing node, it will start to flow down here.
						if (nb.t == NEIGHBOR_LOWER)
							flowing_down = true;
					} else {
						neutrals[num_neutrals++] = nb;
						// If neutral below is ignore prevent water spreading outwards
						if (nb.t == NEIGHBOR_LOWER &&
								nb.n.getContent() == CONTENT_IGNORE)
							flowing_down = true;
					}
					break;
				case LIQUID_SOURCE:
					// if this node is not (yet) of a liquid type, choose the first liquid type we encounter
					if (liquid_kind == CONTENT_AIR)
						liquid_kind = nodemgr->getId(cfnb.liquid_alternative_flowing);
					if (nodemgr->getId(cfnb.liquid_alternative_flowing) != liquid_kind) {
						neutrals[num_neutrals++] = nb;
					} else {
						// Do not count bottom source, it will screw things up
						if(dirs[i].Y != -1)
							sources[num_sources++] = nb;
					}
					break;
				case LIQUID_FLOWING:
					// if this node is not (yet) of a liquid type, choose the first liquid type we encounter
					if (liquid_kind == CONTENT_AIR)
						liquid_kind = nodemgr->getId(cfnb.liquid_alternative_flowing);
					if (nodemgr->getId(cfnb.liquid_alternative_flowing) != liquid_kind) {
						neutrals[num_neutrals++] = nb;
					} else {
						flows[num_flows++] = nb;
						if (nb.t == NEIGHBOR_LOWER)
							flowing_down = true;
					}
					break;
			}
		}

		/*
			decide on the type (and possibly level) of the current node
		 */
		content_t new_node_content;
		s8 new_node_level = -1;
		s8 max_node_level = -1;

		u8 range = nodemgr->get(liquid_kind).liquid_range;
		if (range > LIQUID_LEVEL_MAX + 1)
			range = LIQUID_LEVEL_MAX + 1;

		if ((num_sources >= 2 && nodemgr->get(liquid_kind).liquid_renewable) || liquid_type == LIQUID_SOURCE) {
			// liquid_kind will be set to either the flowing alternative of the node (if it's a liquid)
			// or the flowing alternative of the first of the surrounding sources (if it's air), so
			// it's perfectly safe to use liquid_kind here to determine the new node content.
			new_node_content = nodemgr->getId(nodemgr->get(liquid_kind).liquid_alternative_source);
		} else if (num_sources >= 1 && sources[0].t != NEIGHBOR_LOWER) {
			// liquid_kind is set properly, see above
			max_node_level = new_node_level = LIQUID_LEVEL_MAX;
			if (new_node_level >= (LIQUID_LEVEL_MAX + 1 - range))
				new_node_content = liquid_kind;
			else
				new_node_content = floodable_node;
		} else {
			// no surrounding sources, so get the maximum level that can flow into this node
			for (u16 i = 0; i < num_flows; i++) {
				u8 nb_liquid_level = (flows[i].n.param2 & LIQUID_LEVEL_MASK);
				switch (flows[i].t) {
					case NEIGHBOR_UPPER:
						if (nb_liquid_level + WATER_DROP_BOOST > max_node_level) {
							max_node_level = LIQUID_LEVEL_MAX;
							if (nb_liquid_level + WATER_DROP_BOOST < LIQUID_LEVEL_MAX)
								max_node_level = nb_liquid_level + WATER_DROP_BOOST;
						} else if (nb_liquid_level > max_node_level) {
							max_node_level = nb_liquid_level;
						}
						break;
					case NEIGHBOR_LOWER:
						break;
					case NEIGHBOR_SAME_LEVEL:
						if ((flows[i].n.param2 & LIQUID_FLOW_DOWN_MASK) != LIQUID_FLOW_DOWN_MASK &&
								nb_liquid_level > 0 && nb_liquid_level - 1 > max_node_level)
							max_node_level = nb_liquid_level - 1;
						break;
				}
			}

			u8 viscosity = nodemgr->get(liquid_kind).liquid_viscosity;
			if (viscosity > 1 && max_node_level != liquid_level) {
				// amount to gain, limited by viscosity
				// must be at least 1 in absolute value
				s8 level_inc = max_node_level - liquid_level;
				if (level_inc < -viscosity || level_inc > viscosity)
					new_node_level = liquid_level + level_inc/viscosity;
				else if (level_inc < 0)
					new_node_level = liquid_level - 1;
				else if (level_inc > 0)
					new_node_level = liquid_level + 1;
				if (new_node_level != max_node_level)
					must_reflow.push_back(p0);
			} else {
				new_node_level = max_node_level;
			}

			if (max_node_level >= (LIQUID_LEVEL_MAX + 1 - range))
				new_node_content = liquid_kind;
			else
				new_node_content = floodable_node;

		}

		/*
			check if anything has changed. if not, just continue with the next node.
		 */
		if (new_node_content == n0.getContent() &&
				(nodemgr->get(n0.getContent()).liquid_type != LIQUID_FLOWING ||
				((n0.param2 & LIQUID_LEVEL_MASK) == (u8)new_node_level &&
				((n0.param2 & LIQUID_FLOW_DOWN_MASK) == LIQUID_FLOW_DOWN_MASK)
				== flowing_down)))
			continue;


		/*
			update the current node
		 */
		MapNode n00 = n0;
		//bool flow_down_enabled = (flowing_down && ((n0.param2 & LIQUID_FLOW_DOWN_MASK) != LIQUID_FLOW_DOWN_MASK));
		if (nodemgr->get(new_node_content).liquid_type == LIQUID_FLOWING) {
			// set level to last 3 bits, flowing down bit to 4th bit
			n0.param2 = (flowing_down ? LIQUID_FLOW_DOWN_MASK : 0x00) | (new_node_level & LIQUID_LEVEL_MASK);
		} else {
			// set the liquid level and flow bit to 0
			n0.param2 = ~(LIQUID_LEVEL_MASK | LIQUID_FLOW_DOWN_MASK);
		}
		n0.setContent(new_node_content);

		// Find out whether there is a suspect for this action
		std::string suspect;
		if (m_gamedef->rollback())
			suspect = m_gamedef->rollback()->getSuspect(p0, 83, 1);

		if (m_gamedef->rollback() && !suspect.empty()) {
			// Blame suspect
			RollbackScopeActor rollback_scope(m_gamedef->rollback(), suspect, true);
			// Get old node for rollback
			RollbackNode rollback_oldnode(this, p0, m_gamedef);
			// Set node
			setNode(p0, n0);
			// Report
			RollbackNode rollback_newnode(this, p0, m_gamedef);
			RollbackAction action;
			action.setSetNode(p0, rollback_oldnode, rollback_newnode);
			m_gamedef->rollback()->reportAction(action);
		} else {
			// Set node
			setNode(p0, n0);
		}

		v3s16 blockpos = getNodeBlockPos(p0);
		MapBlock *block = getBlockNoCreateNoEx(blockpos);
		if (block != NULL) {
			modified_blocks[blockpos] =  block;
			// If new or old node emits light, MapBlock requires lighting update
			if (nodemgr->get(n0).light_source != 0 ||
					nodemgr->get(n00).light_source != 0)
				lighting_modified_blocks[block->getPos()] = block;
		}

		/*
			enqueue neighbors for update if neccessary
		 */
		switch (nodemgr->get(n0.getContent()).liquid_type) {
			case LIQUID_SOURCE:
			case LIQUID_FLOWING:
				// make sure source flows into all neighboring nodes
				for (u16 i = 0; i < num_flows; i++)
					if (flows[i].t != NEIGHBOR_UPPER)
						m_transforming_liquid.push_back(flows[i].p);
				for (u16 i = 0; i < num_airs; i++)
					if (airs[i].t != NEIGHBOR_UPPER)
						m_transforming_liquid.push_back(airs[i].p);
				break;
			case LIQUID_NONE:
				// this flow has turned to air; neighboring flows might need to do the same
				for (u16 i = 0; i < num_flows; i++)
					m_transforming_liquid.push_back(flows[i].p);
				break;
		}
	}
	//infostream<<"Map::transformLiquids(): loopcount="<<loopcount<<std::endl;

	for (std::deque<v3s16>::iterator iter = must_reflow.begin(); iter != must_reflow.end(); ++iter)
		m_transforming_liquid.push_back(*iter);

	updateLighting(lighting_modified_blocks, modified_blocks);


	/* ----------------------------------------------------------------------
	 * Manage the queue so that it does not grow indefinately
	 */
	u16 time_until_purge = g_settings->getU16("liquid_queue_purge_time");

	if (time_until_purge == 0)
		return; // Feature disabled

	time_until_purge *= 1000;	// seconds -> milliseconds

	u32 curr_time = getTime(PRECISION_MILLI);
	u32 prev_unprocessed = m_unprocessed_count;
	m_unprocessed_count = m_transforming_liquid.size();

	// if unprocessed block count is decreasing or stable
	if (m_unprocessed_count <= prev_unprocessed) {
		m_queue_size_timer_started = false;
	} else {
		if (!m_queue_size_timer_started)
			m_inc_trending_up_start_time = curr_time;
		m_queue_size_timer_started = true;
	}

	// Account for curr_time overflowing
	if (m_queue_size_timer_started && m_inc_trending_up_start_time > curr_time)
		m_queue_size_timer_started = false;

	/* If the queue has been growing for more than liquid_queue_purge_time seconds
	 * and the number of unprocessed blocks is still > liquid_loop_max then we
	 * cannot keep up; dump the oldest blocks from the queue so that the queue
	 * has liquid_loop_max items in it
	 */
	if (m_queue_size_timer_started
			&& curr_time - m_inc_trending_up_start_time > time_until_purge
			&& m_unprocessed_count > liquid_loop_max) {

		size_t dump_qty = m_unprocessed_count - liquid_loop_max;

		infostream << "transformLiquids(): DUMPING " << dump_qty
		           << " blocks from the queue" << std::endl;

		while (dump_qty--)
			m_transforming_liquid.pop_front();

		m_queue_size_timer_started = false; // optimistically assume we can keep up now
		m_unprocessed_count = m_transforming_liquid.size();
	}
}

std::vector<v3s16> Map::findNodesWithMetadata(v3s16 p1, v3s16 p2)
{
	std::vector<v3s16> positions_with_meta;

	sortBoxVerticies(p1, p2);
	v3s16 bpmin = getNodeBlockPos(p1);
	v3s16 bpmax = getNodeBlockPos(p2);

	VoxelArea area(p1, p2);

	for (s16 z = bpmin.Z; z <= bpmax.Z; z++)
	for (s16 y = bpmin.Y; y <= bpmax.Y; y++)
	for (s16 x = bpmin.X; x <= bpmax.X; x++) {
		v3s16 blockpos(x, y, z);

		MapBlock *block = getBlockNoCreateNoEx(blockpos);
		if (!block) {
			verbosestream << "Map::getNodeMetadata(): Need to emerge "
				<< PP(blockpos) << std::endl;
			block = emergeBlock(blockpos, false);
		}
		if (!block) {
			infostream << "WARNING: Map::getNodeMetadata(): Block not found"
				<< std::endl;
			continue;
		}

		v3s16 p_base = blockpos * MAP_BLOCKSIZE;
		std::vector<v3s16> keys = block->m_node_metadata.getAllKeys();
		for (size_t i = 0; i != keys.size(); i++) {
			v3s16 p(keys[i] + p_base);
			if (!area.contains(p))
				continue;

			positions_with_meta.push_back(p);
		}
	}

	return positions_with_meta;
}

NodeMetadata *Map::getNodeMetadata(v3s16 p)
{
	v3s16 blockpos = getNodeBlockPos(p);
	v3s16 p_rel = p - blockpos*MAP_BLOCKSIZE;
	MapBlock *block = getBlockNoCreateNoEx(blockpos);
	if(!block){
		infostream<<"Map::getNodeMetadata(): Need to emerge "
				<<PP(blockpos)<<std::endl;
		block = emergeBlock(blockpos, false);
	}
	if(!block){
		warningstream<<"Map::getNodeMetadata(): Block not found"
				<<std::endl;
		return NULL;
	}
	NodeMetadata *meta = block->m_node_metadata.get(p_rel);
	return meta;
}

bool Map::setNodeMetadata(v3s16 p, NodeMetadata *meta)
{
	v3s16 blockpos = getNodeBlockPos(p);
	v3s16 p_rel = p - blockpos*MAP_BLOCKSIZE;
	MapBlock *block = getBlockNoCreateNoEx(blockpos);
	if(!block){
		infostream<<"Map::setNodeMetadata(): Need to emerge "
				<<PP(blockpos)<<std::endl;
		block = emergeBlock(blockpos, false);
	}
	if(!block){
		warningstream<<"Map::setNodeMetadata(): Block not found"
				<<std::endl;
		return false;
	}
	block->m_node_metadata.set(p_rel, meta);
	return true;
}

void Map::removeNodeMetadata(v3s16 p)
{
	v3s16 blockpos = getNodeBlockPos(p);
	v3s16 p_rel = p - blockpos*MAP_BLOCKSIZE;
	MapBlock *block = getBlockNoCreateNoEx(blockpos);
	if(block == NULL)
	{
		warningstream<<"Map::removeNodeMetadata(): Block not found"
				<<std::endl;
		return;
	}
	block->m_node_metadata.remove(p_rel);
}

NodeTimer Map::getNodeTimer(v3s16 p)
{
	v3s16 blockpos = getNodeBlockPos(p);
	v3s16 p_rel = p - blockpos*MAP_BLOCKSIZE;
	MapBlock *block = getBlockNoCreateNoEx(blockpos);
	if(!block){
		infostream<<"Map::getNodeTimer(): Need to emerge "
				<<PP(blockpos)<<std::endl;
		block = emergeBlock(blockpos, false);
	}
	if(!block){
		warningstream<<"Map::getNodeTimer(): Block not found"
				<<std::endl;
		return NodeTimer();
	}
	NodeTimer t = block->m_node_timers.get(p_rel);
	return t;
}

void Map::setNodeTimer(v3s16 p, NodeTimer t)
{
	v3s16 blockpos = getNodeBlockPos(p);
	v3s16 p_rel = p - blockpos*MAP_BLOCKSIZE;
	MapBlock *block = getBlockNoCreateNoEx(blockpos);
	if(!block){
		infostream<<"Map::setNodeTimer(): Need to emerge "
				<<PP(blockpos)<<std::endl;
		block = emergeBlock(blockpos, false);
	}
	if(!block){
		warningstream<<"Map::setNodeTimer(): Block not found"
				<<std::endl;
		return;
	}
	block->m_node_timers.set(p_rel, t);
}

void Map::removeNodeTimer(v3s16 p)
{
	v3s16 blockpos = getNodeBlockPos(p);
	v3s16 p_rel = p - blockpos*MAP_BLOCKSIZE;
	MapBlock *block = getBlockNoCreateNoEx(blockpos);
	if(block == NULL)
	{
		warningstream<<"Map::removeNodeTimer(): Block not found"
				<<std::endl;
		return;
	}
	block->m_node_timers.remove(p_rel);
}

/*
	ServerMap
*/
ServerMap::ServerMap(std::string savedir, IGameDef *gamedef, EmergeManager *emerge):
	Map(dout_server, gamedef),
	m_emerge(emerge),
	m_map_metadata_changed(true)
{
	verbosestream<<FUNCTION_NAME<<std::endl;

	/*
		Try to load map; if not found, create a new one.
	*/

	// Determine which database backend to use
	std::string conf_path = savedir + DIR_DELIM + "world.mt";
	Settings conf;
	bool succeeded = conf.readConfigFile(conf_path.c_str());
	if (!succeeded || !conf.exists("backend")) {
		// fall back to sqlite3
		conf.set("backend", "sqlite3");
	}
	std::string backend = conf.get("backend");
	dbase = createDatabase(backend, savedir, conf);

	if (!conf.updateConfigFile(conf_path.c_str()))
		errorstream << "ServerMap::ServerMap(): Failed to update world.mt!" << std::endl;

	m_savedir = savedir;
	m_map_saving_enabled = false;

	try
	{
		// If directory exists, check contents and load if possible
		if(fs::PathExists(m_savedir))
		{
			// If directory is empty, it is safe to save into it.
			if(fs::GetDirListing(m_savedir).size() == 0)
			{
				infostream<<"ServerMap: Empty save directory is valid."
						<<std::endl;
				m_map_saving_enabled = true;
			}
			else
			{
				try{
					// Load map metadata (seed, chunksize)
					loadMapMeta();
				}
				catch(SettingNotFoundException &e){
					infostream<<"ServerMap:  Some metadata not found."
							  <<" Using default settings."<<std::endl;
				}
				catch(FileNotGoodException &e){
					warningstream<<"Could not load map metadata"
							//<<" Disabling chunk-based generator."
							<<std::endl;
					//m_chunksize = 0;
				}

				infostream<<"ServerMap: Successfully loaded map "
						<<"metadata from "<<savedir
						<<", assuming valid save directory."
						<<" seed="<< m_emerge->params.seed <<"."
						<<std::endl;

				m_map_saving_enabled = true;
				// Map loaded, not creating new one
				return;
			}
		}
		// If directory doesn't exist, it is safe to save to it
		else{
			m_map_saving_enabled = true;
		}
	}
	catch(std::exception &e)
	{
		warningstream<<"ServerMap: Failed to load map from "<<savedir
				<<", exception: "<<e.what()<<std::endl;
		infostream<<"Please remove the map or fix it."<<std::endl;
		warningstream<<"Map saving will be disabled."<<std::endl;
	}

	infostream<<"Initializing new map."<<std::endl;

	// Create zero sector
	emergeSector(v2s16(0,0));

	// Initially write whole map
	save(MOD_STATE_CLEAN);
}

ServerMap::~ServerMap()
{
	verbosestream<<FUNCTION_NAME<<std::endl;

	try
	{
		if(m_map_saving_enabled)
		{
			// Save only changed parts
			save(MOD_STATE_WRITE_AT_UNLOAD);
			infostream<<"ServerMap: Saved map to "<<m_savedir<<std::endl;
		}
		else
		{
			infostream<<"ServerMap: Map not saved"<<std::endl;
		}
	}
	catch(std::exception &e)
	{
		infostream<<"ServerMap: Failed to save map to "<<m_savedir
				<<", exception: "<<e.what()<<std::endl;
	}

	/*
		Close database if it was opened
	*/
	delete dbase;

#if 0
	/*
		Free all MapChunks
	*/
	core::map<v2s16, MapChunk*>::Iterator i = m_chunks.getIterator();
	for(; i.atEnd() == false; i++)
	{
		MapChunk *chunk = i.getNode()->getValue();
		delete chunk;
	}
#endif
}

u64 ServerMap::getSeed()
{
	return m_emerge->params.seed;
}

s16 ServerMap::getWaterLevel()
{
	return m_emerge->params.water_level;
}

bool ServerMap::initBlockMake(v3s16 blockpos, BlockMakeData *data)
{
	s16 csize = m_emerge->params.chunksize;
	v3s16 bpmin = EmergeManager::getContainingChunk(blockpos, csize);
	v3s16 bpmax = bpmin + v3s16(1, 1, 1) * (csize - 1);

	bool enable_mapgen_debug_info = m_emerge->enable_mapgen_debug_info;
	EMERGE_DBG_OUT("initBlockMake(): " PP(bpmin) " - " PP(bpmax));

	v3s16 extra_borders(1, 1, 1);
	v3s16 full_bpmin = bpmin - extra_borders;
	v3s16 full_bpmax = bpmax + extra_borders;

	// Do nothing if not inside limits (+-1 because of neighbors)
	if (blockpos_over_limit(full_bpmin) ||
		blockpos_over_limit(full_bpmax))
		return false;

	data->seed = m_emerge->params.seed;
	data->blockpos_min = bpmin;
	data->blockpos_max = bpmax;
	data->blockpos_requested = blockpos;
	data->nodedef = m_gamedef->ndef();

	/*
		Create the whole area of this and the neighboring blocks
	*/
	for (s16 x = full_bpmin.X; x <= full_bpmax.X; x++)
	for (s16 z = full_bpmin.Z; z <= full_bpmax.Z; z++) {
		v2s16 sectorpos(x, z);
		// Sector metadata is loaded from disk if not already loaded.
		ServerMapSector *sector = createSector(sectorpos);
		FATAL_ERROR_IF(sector == NULL, "createSector() failed");

		for (s16 y = full_bpmin.Y; y <= full_bpmax.Y; y++) {
			v3s16 p(x, y, z);

			MapBlock *block = emergeBlock(p, false);
			if (block == NULL) {
				block = createBlock(p);

				// Block gets sunlight if this is true.
				// Refer to the map generator heuristics.
				bool ug = m_emerge->isBlockUnderground(p);
				block->setIsUnderground(ug);
			}
		}
	}

	/*
		Now we have a big empty area.

		Make a ManualMapVoxelManipulator that contains this and the
		neighboring blocks
	*/

	data->vmanip = new MMVManip(this);
	data->vmanip->initialEmerge(full_bpmin, full_bpmax);

	// Note: we may need this again at some point.
#if 0
	// Ensure none of the blocks to be generated were marked as
	// containing CONTENT_IGNORE
	for (s16 z = blockpos_min.Z; z <= blockpos_max.Z; z++) {
		for (s16 y = blockpos_min.Y; y <= blockpos_max.Y; y++) {
			for (s16 x = blockpos_min.X; x <= blockpos_max.X; x++) {
				core::map<v3s16, u8>::Node *n;
				n = data->vmanip->m_loaded_blocks.find(v3s16(x, y, z));
				if (n == NULL)
					continue;
				u8 flags = n->getValue();
				flags &= ~VMANIP_BLOCK_CONTAINS_CIGNORE;
				n->setValue(flags);
			}
		}
	}
#endif

	// Data is ready now.
	return true;
}

void ServerMap::finishBlockMake(BlockMakeData *data,
	std::map<v3s16, MapBlock*> *changed_blocks)
{
	v3s16 bpmin = data->blockpos_min;
	v3s16 bpmax = data->blockpos_max;

	v3s16 extra_borders(1, 1, 1);
	v3s16 full_bpmin = bpmin - extra_borders;
	v3s16 full_bpmax = bpmax + extra_borders;

	bool enable_mapgen_debug_info = m_emerge->enable_mapgen_debug_info;
	EMERGE_DBG_OUT("finishBlockMake(): " PP(bpmin) " - " PP(bpmax));

	/*
		Set lighting to non-expired state in all of them.
		This is cheating, but it is not fast enough if all of them
		would actually be updated.
	*/
	for (s16 x = full_bpmin.X; x <= full_bpmax.X; x++)
	for (s16 z = full_bpmin.Z; z <= full_bpmax.Z; z++)
	for (s16 y = full_bpmin.Y; y <= full_bpmax.Y; y++) {
		MapBlock *block = emergeBlock(v3s16(x, y, z), false);
		if (!block)
			continue;

		block->setLightingExpired(false);
	}

	/*
		Blit generated stuff to map
		NOTE: blitBackAll adds nearly everything to changed_blocks
	*/
	data->vmanip->blitBackAll(changed_blocks);

	EMERGE_DBG_OUT("finishBlockMake: changed_blocks.size()="
		<< changed_blocks->size());

	/*
		Copy transforming liquid information
	*/
	while (data->transforming_liquid.size()) {
		m_transforming_liquid.push_back(data->transforming_liquid.front());
		data->transforming_liquid.pop_front();
	}

	for (std::map<v3s16, MapBlock *>::iterator
			it = changed_blocks->begin();
			it != changed_blocks->end(); ++it) {
		MapBlock *block = it->second;
		if (!block)
			continue;
		/*
			Update day/night difference cache of the MapBlocks
		*/
		block->expireDayNightDiff();
		/*
			Set block as modified
		*/
		block->raiseModified(MOD_STATE_WRITE_NEEDED,
			MOD_REASON_EXPIRE_DAYNIGHTDIFF);
	}

	/*
		Set central blocks as generated
	*/
	for (s16 x = bpmin.X; x <= bpmax.X; x++)
	for (s16 z = bpmin.Z; z <= bpmax.Z; z++)
	for (s16 y = bpmin.Y; y <= bpmax.Y; y++) {
		MapBlock *block = getBlockNoCreateNoEx(v3s16(x, y, z));
		if (!block)
			continue;

		block->setGenerated(true);
	}

	/*
		Save changed parts of map
		NOTE: Will be saved later.
	*/
	//save(MOD_STATE_WRITE_AT_UNLOAD);
}

ServerMapSector *ServerMap::createSector(v2s16 p2d)
{
	DSTACKF("%s: p2d=(%d,%d)",
			FUNCTION_NAME,
			p2d.X, p2d.Y);

	/*
		Check if it exists already in memory
	*/
	ServerMapSector *sector = (ServerMapSector*)getSectorNoGenerateNoEx(p2d);
	if(sector != NULL)
		return sector;

	/*
		Try to load it from disk (with blocks)
	*/
	//if(loadSectorFull(p2d) == true)

	/*
		Try to load metadata from disk
	*/
#if 0
	if(loadSectorMeta(p2d) == true)
	{
		ServerMapSector *sector = (ServerMapSector*)getSectorNoGenerateNoEx(p2d);
		if(sector == NULL)
		{
			infostream<<"ServerMap::createSector(): loadSectorFull didn't make a sector"<<std::endl;
			throw InvalidPositionException("");
		}
		return sector;
	}
#endif
	/*
		Do not create over-limit
	*/
	const static u16 map_gen_limit = MYMIN(MAX_MAP_GENERATION_LIMIT,
		g_settings->getU16("map_generation_limit"));
	if(p2d.X < -map_gen_limit / MAP_BLOCKSIZE
			|| p2d.X >  map_gen_limit / MAP_BLOCKSIZE
			|| p2d.Y < -map_gen_limit / MAP_BLOCKSIZE
			|| p2d.Y >  map_gen_limit / MAP_BLOCKSIZE)
		throw InvalidPositionException("createSector(): pos. over limit");

	/*
		Generate blank sector
	*/

	sector = new ServerMapSector(this, p2d, m_gamedef);

	// Sector position on map in nodes
	//v2s16 nodepos2d = p2d * MAP_BLOCKSIZE;

	/*
		Insert to container
	*/
	m_sectors[p2d] = sector;

	return sector;
}

#if 0
/*
	This is a quick-hand function for calling makeBlock().
*/
MapBlock * ServerMap::generateBlock(
		v3s16 p,
		std::map<v3s16, MapBlock*> &modified_blocks
)
{
	DSTACKF("%s: p=(%d,%d,%d)", FUNCTION_NAME, p.X, p.Y, p.Z);

	/*infostream<<"generateBlock(): "
			<<"("<<p.X<<","<<p.Y<<","<<p.Z<<")"
			<<std::endl;*/

	bool enable_mapgen_debug_info = g_settings->getBool("enable_mapgen_debug_info");

	TimeTaker timer("generateBlock");

	//MapBlock *block = original_dummy;

	v2s16 p2d(p.X, p.Z);
	v2s16 p2d_nodes = p2d * MAP_BLOCKSIZE;

	/*
		Do not generate over-limit
	*/
	if(blockpos_over_limit(p))
	{
		infostream<<FUNCTION_NAME<<": Block position over limit"<<std::endl;
		throw InvalidPositionException("generateBlock(): pos. over limit");
	}

	/*
		Create block make data
	*/
	BlockMakeData data;
	initBlockMake(&data, p);

	/*
		Generate block
	*/
	{
		TimeTaker t("mapgen::make_block()");
		mapgen->makeChunk(&data);
		//mapgen::make_block(&data);

		if(enable_mapgen_debug_info == false)
			t.stop(true); // Hide output
	}

	/*
		Blit data back on map, update lighting, add mobs and whatever this does
	*/
	finishBlockMake(&data, modified_blocks);

	/*
		Get central block
	*/
	MapBlock *block = getBlockNoCreateNoEx(p);

#if 0
	/*
		Check result
	*/
	if(block)
	{
		bool erroneus_content = false;
		for(s16 z0=0; z0<MAP_BLOCKSIZE; z0++)
		for(s16 y0=0; y0<MAP_BLOCKSIZE; y0++)
		for(s16 x0=0; x0<MAP_BLOCKSIZE; x0++)
		{
			v3s16 p(x0,y0,z0);
			MapNode n = block->getNode(p);
			if(n.getContent() == CONTENT_IGNORE)
			{
				infostream<<"CONTENT_IGNORE at "
						<<"("<<p.X<<","<<p.Y<<","<<p.Z<<")"
						<<std::endl;
				erroneus_content = true;
				assert(0);
			}
		}
		if(erroneus_content)
		{
			assert(0);
		}
	}
#endif

#if 0
	/*
		Generate a completely empty block
	*/
	if(block)
	{
		for(s16 z0=0; z0<MAP_BLOCKSIZE; z0++)
		for(s16 x0=0; x0<MAP_BLOCKSIZE; x0++)
		{
			for(s16 y0=0; y0<MAP_BLOCKSIZE; y0++)
			{
				MapNode n;
				n.setContent(CONTENT_AIR);
				block->setNode(v3s16(x0,y0,z0), n);
			}
		}
	}
#endif

	if(enable_mapgen_debug_info == false)
		timer.stop(true); // Hide output

	return block;
}
#endif

MapBlock * ServerMap::createBlock(v3s16 p)
{
	DSTACKF("%s: p=(%d,%d,%d)",
			FUNCTION_NAME, p.X, p.Y, p.Z);

	/*
		Do not create over-limit
	*/
	if (blockpos_over_limit(p))
		throw InvalidPositionException("createBlock(): pos. over limit");

	v2s16 p2d(p.X, p.Z);
	s16 block_y = p.Y;
	/*
		This will create or load a sector if not found in memory.
		If block exists on disk, it will be loaded.

		NOTE: On old save formats, this will be slow, as it generates
		      lighting on blocks for them.
	*/
	ServerMapSector *sector;
	try {
		sector = (ServerMapSector*)createSector(p2d);
		assert(sector->getId() == MAPSECTOR_SERVER);
	}
	catch(InvalidPositionException &e)
	{
		infostream<<"createBlock: createSector() failed"<<std::endl;
		throw e;
	}
	/*
		NOTE: This should not be done, or at least the exception
		should not be passed on as std::exception, because it
		won't be catched at all.
	*/
	/*catch(std::exception &e)
	{
		infostream<<"createBlock: createSector() failed: "
				<<e.what()<<std::endl;
		throw e;
	}*/

	/*
		Try to get a block from the sector
	*/

	MapBlock *block = sector->getBlockNoCreateNoEx(block_y);
	if(block)
	{
		if(block->isDummy())
			block->unDummify();
		return block;
	}
	// Create blank
	block = sector->createBlankBlock(block_y);

	return block;
}

MapBlock * ServerMap::emergeBlock(v3s16 p, bool create_blank)
{
	DSTACKF("%s: p=(%d,%d,%d), create_blank=%d",
			FUNCTION_NAME,
			p.X, p.Y, p.Z, create_blank);

	{
		MapBlock *block = getBlockNoCreateNoEx(p);
		if(block && block->isDummy() == false)
			return block;
	}

	{
		MapBlock *block = loadBlock(p);
		if(block)
			return block;
	}

	if (create_blank) {
		ServerMapSector *sector = createSector(v2s16(p.X, p.Z));
		MapBlock *block = sector->createBlankBlock(p.Y);

		return block;
	}

#if 0
	if(allow_generate)
	{
		std::map<v3s16, MapBlock*> modified_blocks;
		MapBlock *block = generateBlock(p, modified_blocks);
		if(block)
		{
			MapEditEvent event;
			event.type = MEET_OTHER;
			event.p = p;

			// Copy modified_blocks to event
			for(std::map<v3s16, MapBlock*>::iterator
					i = modified_blocks.begin();
					i != modified_blocks.end(); ++i)
			{
				event.modified_blocks.insert(i->first);
			}

			// Queue event
			dispatchEvent(&event);

			return block;
		}
	}
#endif

	return NULL;
}

MapBlock *ServerMap::getBlockOrEmerge(v3s16 p3d)
{
	MapBlock *block = getBlockNoCreateNoEx(p3d);
	if (block == NULL)
		m_emerge->enqueueBlockEmerge(PEER_ID_INEXISTENT, p3d, false);

	return block;
}

void ServerMap::prepareBlock(MapBlock *block) {
}

// N.B.  This requires no synchronization, since data will not be modified unless
// the VoxelManipulator being updated belongs to the same thread.
void ServerMap::updateVManip(v3s16 pos)
{
	Mapgen *mg = m_emerge->getCurrentMapgen();
	if (!mg)
		return;

	MMVManip *vm = mg->vm;
	if (!vm)
		return;

	if (!vm->m_area.contains(pos))
		return;

	s32 idx = vm->m_area.index(pos);
	vm->m_data[idx] = getNodeNoEx(pos);
	vm->m_flags[idx] &= ~VOXELFLAG_NO_DATA;

	vm->m_is_dirty = true;
}

s16 ServerMap::findGroundLevel(v2s16 p2d)
{
#if 0
	/*
		Uh, just do something random...
	*/
	// Find existing map from top to down
	s16 max=63;
	s16 min=-64;
	v3s16 p(p2d.X, max, p2d.Y);
	for(; p.Y>min; p.Y--)
	{
		MapNode n = getNodeNoEx(p);
		if(n.getContent() != CONTENT_IGNORE)
			break;
	}
	if(p.Y == min)
		goto plan_b;
	// If this node is not air, go to plan b
	if(getNodeNoEx(p).getContent() != CONTENT_AIR)
		goto plan_b;
	// Search existing walkable and return it
	for(; p.Y>min; p.Y--)
	{
		MapNode n = getNodeNoEx(p);
		if(content_walkable(n.d) && n.getContent() != CONTENT_IGNORE)
			return p.Y;
	}

	// Move to plan b
plan_b:
#endif

	/*
		Determine from map generator noise functions
	*/

	s16 level = m_emerge->getGroundLevelAtPoint(p2d);
	return level;

	//double level = base_rock_level_2d(m_seed, p2d) + AVERAGE_MUD_AMOUNT;
	//return (s16)level;
}

bool ServerMap::loadFromFolders() {
	if (!dbase->initialized() &&
			!fs::PathExists(m_savedir + DIR_DELIM + "map.sqlite"))
		return true;
	return false;
}

void ServerMap::createDirs(std::string path)
{
	if(fs::CreateAllDirs(path) == false)
	{
		m_dout<<"ServerMap: Failed to create directory "
				<<"\""<<path<<"\""<<std::endl;
		throw BaseException("ServerMap failed to create directory");
	}
}

std::string ServerMap::getSectorDir(v2s16 pos, int layout)
{
	char cc[9];
	switch(layout)
	{
		case 1:
			snprintf(cc, 9, "%.4x%.4x",
				(unsigned int) pos.X & 0xffff,
				(unsigned int) pos.Y & 0xffff);

			return m_savedir + DIR_DELIM + "sectors" + DIR_DELIM + cc;
		case 2:
			snprintf(cc, 9, (std::string("%.3x") + DIR_DELIM + "%.3x").c_str(),
				(unsigned int) pos.X & 0xfff,
				(unsigned int) pos.Y & 0xfff);

			return m_savedir + DIR_DELIM + "sectors2" + DIR_DELIM + cc;
		default:
			assert(false);
			return "";
	}
}

v2s16 ServerMap::getSectorPos(std::string dirname)
{
	unsigned int x = 0, y = 0;
	int r;
	std::string component;
	fs::RemoveLastPathComponent(dirname, &component, 1);
	if(component.size() == 8)
	{
		// Old layout
		r = sscanf(component.c_str(), "%4x%4x", &x, &y);
	}
	else if(component.size() == 3)
	{
		// New layout
		fs::RemoveLastPathComponent(dirname, &component, 2);
		r = sscanf(component.c_str(), (std::string("%3x") + DIR_DELIM + "%3x").c_str(), &x, &y);
		// Sign-extend the 12 bit values up to 16 bits...
		if(x & 0x800) x |= 0xF000;
		if(y & 0x800) y |= 0xF000;
	}
	else
	{
		r = -1;
	}

	FATAL_ERROR_IF(r != 2, "getSectorPos()");
	v2s16 pos((s16)x, (s16)y);
	return pos;
}

v3s16 ServerMap::getBlockPos(std::string sectordir, std::string blockfile)
{
	v2s16 p2d = getSectorPos(sectordir);

	if(blockfile.size() != 4){
		throw InvalidFilenameException("Invalid block filename");
	}
	unsigned int y;
	int r = sscanf(blockfile.c_str(), "%4x", &y);
	if(r != 1)
		throw InvalidFilenameException("Invalid block filename");
	return v3s16(p2d.X, y, p2d.Y);
}

std::string ServerMap::getBlockFilename(v3s16 p)
{
	char cc[5];
	snprintf(cc, 5, "%.4x", (unsigned int)p.Y&0xffff);
	return cc;
}

void ServerMap::save(ModifiedState save_level)
{
	DSTACK(FUNCTION_NAME);
	if(m_map_saving_enabled == false) {
		warningstream<<"Not saving map, saving disabled."<<std::endl;
		return;
	}

	if(save_level == MOD_STATE_CLEAN)
		infostream<<"ServerMap: Saving whole map, this can take time."
				<<std::endl;

	if(m_map_metadata_changed || save_level == MOD_STATE_CLEAN) {
		saveMapMeta();
	}

	// Profile modified reasons
	Profiler modprofiler;

	u32 sector_meta_count = 0;
	u32 block_count = 0;
	u32 block_count_all = 0; // Number of blocks in memory

	// Don't do anything with sqlite unless something is really saved
	bool save_started = false;

	for(std::map<v2s16, MapSector*>::iterator i = m_sectors.begin();
		i != m_sectors.end(); ++i) {
		ServerMapSector *sector = (ServerMapSector*)i->second;
		assert(sector->getId() == MAPSECTOR_SERVER);

		if(sector->differs_from_disk || save_level == MOD_STATE_CLEAN) {
			saveSectorMeta(sector);
			sector_meta_count++;
		}

		MapBlockVect blocks;
		sector->getBlocks(blocks);

		for(MapBlockVect::iterator j = blocks.begin();
			j != blocks.end(); ++j) {
			MapBlock *block = *j;

			block_count_all++;

			if(block->getModified() >= (u32)save_level) {
				// Lazy beginSave()
				if(!save_started) {
					beginSave();
					save_started = true;
				}

				modprofiler.add(block->getModifiedReasonString(), 1);

				saveBlock(block);
				block_count++;

				/*infostream<<"ServerMap: Written block ("
						<<block->getPos().X<<","
						<<block->getPos().Y<<","
						<<block->getPos().Z<<")"
						<<std::endl;*/
			}
		}
	}

	if(save_started)
		endSave();

	/*
		Only print if something happened or saved whole map
	*/
	if(save_level == MOD_STATE_CLEAN || sector_meta_count != 0
			|| block_count != 0) {
		infostream<<"ServerMap: Written: "
				<<sector_meta_count<<" sector metadata files, "
				<<block_count<<" block files"
				<<", "<<block_count_all<<" blocks in memory."
				<<std::endl;
		PrintInfo(infostream); // ServerMap/ClientMap:
		infostream<<"Blocks modified by: "<<std::endl;
		modprofiler.print(infostream);
	}
}

void ServerMap::listAllLoadableBlocks(std::vector<v3s16> &dst)
{
	if (loadFromFolders()) {
		errorstream << "Map::listAllLoadableBlocks(): Result will be missing "
				<< "all blocks that are stored in flat files." << std::endl;
	}
	dbase->listAllLoadableBlocks(dst);
}

void ServerMap::listAllLoadedBlocks(std::vector<v3s16> &dst)
{
	for(std::map<v2s16, MapSector*>::iterator si = m_sectors.begin();
		si != m_sectors.end(); ++si)
	{
		MapSector *sector = si->second;

		MapBlockVect blocks;
		sector->getBlocks(blocks);

		for(MapBlockVect::iterator i = blocks.begin();
				i != blocks.end(); ++i) {
			v3s16 p = (*i)->getPos();
			dst.push_back(p);
		}
	}
}

void ServerMap::saveMapMeta()
{
	DSTACK(FUNCTION_NAME);

	createDirs(m_savedir);

	std::string fullpath = m_savedir + DIR_DELIM + "map_meta.txt";
	std::ostringstream oss(std::ios_base::binary);
	Settings conf;

	m_emerge->params.save(conf);
	conf.writeLines(oss);

	oss << "[end_of_params]\n";

	if(!fs::safeWriteToFile(fullpath, oss.str())) {
		errorstream << "ServerMap::saveMapMeta(): "
				<< "could not write " << fullpath << std::endl;
		throw FileNotGoodException("Cannot save chunk metadata");
	}

	m_map_metadata_changed = false;
}

void ServerMap::loadMapMeta()
{
	DSTACK(FUNCTION_NAME);

	Settings conf;
	std::string fullpath = m_savedir + DIR_DELIM + "map_meta.txt";

	std::ifstream is(fullpath.c_str(), std::ios_base::binary);
	if (!is.good()) {
		errorstream << "ServerMap::loadMapMeta(): "
			"could not open " << fullpath << std::endl;
		throw FileNotGoodException("Cannot open map metadata");
	}

	if (!conf.parseConfigLines(is, "[end_of_params]")) {
		throw SerializationError("ServerMap::loadMapMeta(): "
				"[end_of_params] not found!");
	}

	m_emerge->params.load(conf);

	verbosestream << "ServerMap::loadMapMeta(): seed="
		<< m_emerge->params.seed << std::endl;
}

void ServerMap::saveSectorMeta(ServerMapSector *sector)
{
	DSTACK(FUNCTION_NAME);
	// Format used for writing
	u8 version = SER_FMT_VER_HIGHEST_WRITE;
	// Get destination
	v2s16 pos = sector->getPos();
	std::string dir = getSectorDir(pos);
	createDirs(dir);

	std::string fullpath = dir + DIR_DELIM + "meta";
	std::ostringstream ss(std::ios_base::binary);

	sector->serialize(ss, version);

	if(!fs::safeWriteToFile(fullpath, ss.str()))
		throw FileNotGoodException("Cannot write sector metafile");

	sector->differs_from_disk = false;
}

MapSector* ServerMap::loadSectorMeta(std::string sectordir, bool save_after_load)
{
	DSTACK(FUNCTION_NAME);
	// Get destination
	v2s16 p2d = getSectorPos(sectordir);

	ServerMapSector *sector = NULL;

	std::string fullpath = sectordir + DIR_DELIM + "meta";
	std::ifstream is(fullpath.c_str(), std::ios_base::binary);
	if(is.good() == false)
	{
		// If the directory exists anyway, it probably is in some old
		// format. Just go ahead and create the sector.
		if(fs::PathExists(sectordir))
		{
			/*infostream<<"ServerMap::loadSectorMeta(): Sector metafile "
					<<fullpath<<" doesn't exist but directory does."
					<<" Continuing with a sector with no metadata."
					<<std::endl;*/
			sector = new ServerMapSector(this, p2d, m_gamedef);
			m_sectors[p2d] = sector;
		}
		else
		{
			throw FileNotGoodException("Cannot open sector metafile");
		}
	}
	else
	{
		sector = ServerMapSector::deSerialize
				(is, this, p2d, m_sectors, m_gamedef);
		if(save_after_load)
			saveSectorMeta(sector);
	}

	sector->differs_from_disk = false;

	return sector;
}

bool ServerMap::loadSectorMeta(v2s16 p2d)
{
	DSTACK(FUNCTION_NAME);

	// The directory layout we're going to load from.
	//  1 - original sectors/xxxxzzzz/
	//  2 - new sectors2/xxx/zzz/
	//  If we load from anything but the latest structure, we will
	//  immediately save to the new one, and remove the old.
	int loadlayout = 1;
	std::string sectordir1 = getSectorDir(p2d, 1);
	std::string sectordir;
	if(fs::PathExists(sectordir1))
	{
		sectordir = sectordir1;
	}
	else
	{
		loadlayout = 2;
		sectordir = getSectorDir(p2d, 2);
	}

	try{
		loadSectorMeta(sectordir, loadlayout != 2);
	}
	catch(InvalidFilenameException &e)
	{
		return false;
	}
	catch(FileNotGoodException &e)
	{
		return false;
	}
	catch(std::exception &e)
	{
		return false;
	}

	return true;
}

#if 0
bool ServerMap::loadSectorFull(v2s16 p2d)
{
	DSTACK(FUNCTION_NAME);

	MapSector *sector = NULL;

	// The directory layout we're going to load from.
	//  1 - original sectors/xxxxzzzz/
	//  2 - new sectors2/xxx/zzz/
	//  If we load from anything but the latest structure, we will
	//  immediately save to the new one, and remove the old.
	int loadlayout = 1;
	std::string sectordir1 = getSectorDir(p2d, 1);
	std::string sectordir;
	if(fs::PathExists(sectordir1))
	{
		sectordir = sectordir1;
	}
	else
	{
		loadlayout = 2;
		sectordir = getSectorDir(p2d, 2);
	}

	try{
		sector = loadSectorMeta(sectordir, loadlayout != 2);
	}
	catch(InvalidFilenameException &e)
	{
		return false;
	}
	catch(FileNotGoodException &e)
	{
		return false;
	}
	catch(std::exception &e)
	{
		return false;
	}

	/*
		Load blocks
	*/
	std::vector<fs::DirListNode> list2 = fs::GetDirListing
			(sectordir);
	std::vector<fs::DirListNode>::iterator i2;
	for(i2=list2.begin(); i2!=list2.end(); i2++)
	{
		// We want files
		if(i2->dir)
			continue;
		try{
			loadBlock(sectordir, i2->name, sector, loadlayout != 2);
		}
		catch(InvalidFilenameException &e)
		{
			// This catches unknown crap in directory
		}
	}

	if(loadlayout != 2)
	{
		infostream<<"Sector converted to new layout - deleting "<<
			sectordir1<<std::endl;
		fs::RecursiveDelete(sectordir1);
	}

	return true;
}
#endif

Database *ServerMap::createDatabase(
	const std::string &name,
	const std::string &savedir,
	Settings &conf)
{
	if (name == "sqlite3")
		return new Database_SQLite3(savedir);
	if (name == "dummy")
		return new Database_Dummy();
	#if USE_LEVELDB
	else if (name == "leveldb")
		return new Database_LevelDB(savedir);
	#endif
	#if USE_REDIS
	else if (name == "redis")
		return new Database_Redis(conf);
	#endif
	else
		throw BaseException(std::string("Database backend ") + name + " not supported.");
}

void ServerMap::beginSave()
{
	dbase->beginSave();
}

void ServerMap::endSave()
{
	dbase->endSave();
}

bool ServerMap::saveBlock(MapBlock *block)
{
	return saveBlock(block, dbase);
}

bool ServerMap::saveBlock(MapBlock *block, Database *db)
{
	v3s16 p3d = block->getPos();

	// Dummy blocks are not written
	if (block->isDummy()) {
		warningstream << "saveBlock: Not writing dummy block "
			<< PP(p3d) << std::endl;
		return true;
	}

	// Format used for writing
	u8 version = SER_FMT_VER_HIGHEST_WRITE;

	/*
		[0] u8 serialization version
		[1] data
	*/
	std::ostringstream o(std::ios_base::binary);
	o.write((char*) &version, 1);
	block->serialize(o, version, true);

	std::string data = o.str();
	bool ret = db->saveBlock(p3d, data);
	if (ret) {
		// We just wrote it to the disk so clear modified flag
		block->resetModified();
	}
	return ret;
}

void ServerMap::loadBlock(std::string sectordir, std::string blockfile,
		MapSector *sector, bool save_after_load)
{
	DSTACK(FUNCTION_NAME);

	std::string fullpath = sectordir + DIR_DELIM + blockfile;
	try {

		std::ifstream is(fullpath.c_str(), std::ios_base::binary);
		if(is.good() == false)
			throw FileNotGoodException("Cannot open block file");

		v3s16 p3d = getBlockPos(sectordir, blockfile);
		v2s16 p2d(p3d.X, p3d.Z);

		assert(sector->getPos() == p2d);

		u8 version = SER_FMT_VER_INVALID;
		is.read((char*)&version, 1);

		if(is.fail())
			throw SerializationError("ServerMap::loadBlock(): Failed"
					" to read MapBlock version");

		/*u32 block_size = MapBlock::serializedLength(version);
		SharedBuffer<u8> data(block_size);
		is.read((char*)*data, block_size);*/

		// This will always return a sector because we're the server
		//MapSector *sector = emergeSector(p2d);

		MapBlock *block = NULL;
		bool created_new = false;
		block = sector->getBlockNoCreateNoEx(p3d.Y);
		if(block == NULL)
		{
			block = sector->createBlankBlockNoInsert(p3d.Y);
			created_new = true;
		}

		// Read basic data
		block->deSerialize(is, version, true);

		// If it's a new block, insert it to the map
		if(created_new)
			sector->insertBlock(block);

		/*
			Save blocks loaded in old format in new format
		*/

		if(version < SER_FMT_VER_HIGHEST_WRITE || save_after_load)
		{
			saveBlock(block);

			// Should be in database now, so delete the old file
			fs::RecursiveDelete(fullpath);
		}

		// We just loaded it from the disk, so it's up-to-date.
		block->resetModified();

	}
	catch(SerializationError &e)
	{
		warningstream<<"Invalid block data on disk "
				<<"fullpath="<<fullpath
				<<" (SerializationError). "
				<<"what()="<<e.what()
				<<std::endl;
				// Ignoring. A new one will be generated.
		abort();

		// TODO: Backup file; name is in fullpath.
	}
}

void ServerMap::loadBlock(std::string *blob, v3s16 p3d, MapSector *sector, bool save_after_load)
{
	DSTACK(FUNCTION_NAME);

	try {
		std::istringstream is(*blob, std::ios_base::binary);

		u8 version = SER_FMT_VER_INVALID;
		is.read((char*)&version, 1);

		if(is.fail())
			throw SerializationError("ServerMap::loadBlock(): Failed"
					" to read MapBlock version");

		/*u32 block_size = MapBlock::serializedLength(version);
		SharedBuffer<u8> data(block_size);
		is.read((char*)*data, block_size);*/

		// This will always return a sector because we're the server
		//MapSector *sector = emergeSector(p2d);

		MapBlock *block = NULL;
		bool created_new = false;
		block = sector->getBlockNoCreateNoEx(p3d.Y);
		if(block == NULL)
		{
			block = sector->createBlankBlockNoInsert(p3d.Y);
			created_new = true;
		}

		// Read basic data
		block->deSerialize(is, version, true);

		// If it's a new block, insert it to the map
		if(created_new)
			sector->insertBlock(block);

		/*
			Save blocks loaded in old format in new format
		*/

		//if(version < SER_FMT_VER_HIGHEST_READ || save_after_load)
		// Only save if asked to; no need to update version
		if(save_after_load)
			saveBlock(block);

		// We just loaded it from, so it's up-to-date.
		block->resetModified();

	}
	catch(SerializationError &e)
	{
		errorstream<<"Invalid block data in database"
				<<" ("<<p3d.X<<","<<p3d.Y<<","<<p3d.Z<<")"
				<<" (SerializationError): "<<e.what()<<std::endl;

		// TODO: Block should be marked as invalid in memory so that it is
		// not touched but the game can run

		if(g_settings->getBool("ignore_world_load_errors")){
			errorstream<<"Ignoring block load error. Duck and cover! "
					<<"(ignore_world_load_errors)"<<std::endl;
		} else {
			throw SerializationError("Invalid block data in database");
		}
	}
}

MapBlock* ServerMap::loadBlock(v3s16 blockpos)
{
	DSTACK(FUNCTION_NAME);

	v2s16 p2d(blockpos.X, blockpos.Z);

	std::string ret;
	dbase->loadBlock(blockpos, &ret);
	if (ret != "") {
		loadBlock(&ret, blockpos, createSector(p2d), false);
		return getBlockNoCreateNoEx(blockpos);
	}
	// Not found in database, try the files

	// The directory layout we're going to load from.
	//  1 - original sectors/xxxxzzzz/
	//  2 - new sectors2/xxx/zzz/
	//  If we load from anything but the latest structure, we will
	//  immediately save to the new one, and remove the old.
	int loadlayout = 1;
	std::string sectordir1 = getSectorDir(p2d, 1);
	std::string sectordir;
	if(fs::PathExists(sectordir1))
	{
		sectordir = sectordir1;
	}
	else
	{
		loadlayout = 2;
		sectordir = getSectorDir(p2d, 2);
	}

	/*
		Make sure sector is loaded
	*/

	MapSector *sector = getSectorNoGenerateNoEx(p2d);
	if(sector == NULL)
	{
		try{
			sector = loadSectorMeta(sectordir, loadlayout != 2);
		}
		catch(InvalidFilenameException &e)
		{
			return NULL;
		}
		catch(FileNotGoodException &e)
		{
			return NULL;
		}
		catch(std::exception &e)
		{
			return NULL;
		}
	}

	/*
		Make sure file exists
	*/

	std::string blockfilename = getBlockFilename(blockpos);
	if(fs::PathExists(sectordir + DIR_DELIM + blockfilename) == false)
		return NULL;

	/*
		Load block and save it to the database
	*/
	loadBlock(sectordir, blockfilename, sector, true);
	return getBlockNoCreateNoEx(blockpos);
}

bool ServerMap::deleteBlock(v3s16 blockpos)
{
	if (!dbase->deleteBlock(blockpos))
		return false;

	MapBlock *block = getBlockNoCreateNoEx(blockpos);
	if (block) {
		v2s16 p2d(blockpos.X, blockpos.Z);
		MapSector *sector = getSectorNoGenerateNoEx(p2d);
		if (!sector)
			return false;
		sector->deleteBlock(block);
	}

	return true;
}

void ServerMap::PrintInfo(std::ostream &out)
{
	out<<"ServerMap: ";
}

MMVManip::MMVManip(Map *map):
		VoxelManipulator(),
		m_is_dirty(false),
		m_create_area(false),
		m_map(map)
{
}

MMVManip::~MMVManip()
{
}

void MMVManip::initialEmerge(v3s16 blockpos_min, v3s16 blockpos_max,
	bool load_if_inexistent)
{
	TimeTaker timer1("initialEmerge", &emerge_time);

	// Units of these are MapBlocks
	v3s16 p_min = blockpos_min;
	v3s16 p_max = blockpos_max;

	VoxelArea block_area_nodes
			(p_min*MAP_BLOCKSIZE, (p_max+1)*MAP_BLOCKSIZE-v3s16(1,1,1));

	u32 size_MB = block_area_nodes.getVolume()*4/1000000;
	if(size_MB >= 1)
	{
		infostream<<"initialEmerge: area: ";
		block_area_nodes.print(infostream);
		infostream<<" ("<<size_MB<<"MB)";
		infostream<<std::endl;
	}

	addArea(block_area_nodes);

	for(s32 z=p_min.Z; z<=p_max.Z; z++)
	for(s32 y=p_min.Y; y<=p_max.Y; y++)
	for(s32 x=p_min.X; x<=p_max.X; x++)
	{
		u8 flags = 0;
		MapBlock *block;
		v3s16 p(x,y,z);
		std::map<v3s16, u8>::iterator n;
		n = m_loaded_blocks.find(p);
		if(n != m_loaded_blocks.end())
			continue;

		bool block_data_inexistent = false;
		try
		{
			TimeTaker timer1("emerge load", &emerge_load_time);

			block = m_map->getBlockNoCreate(p);
			if(block->isDummy())
				block_data_inexistent = true;
			else
				block->copyTo(*this);
		}
		catch(InvalidPositionException &e)
		{
			block_data_inexistent = true;
		}

		if(block_data_inexistent)
		{

			if (load_if_inexistent) {
				ServerMap *svrmap = (ServerMap *)m_map;
				block = svrmap->emergeBlock(p, false);
				if (block == NULL)
					block = svrmap->createBlock(p);
				block->copyTo(*this);
			} else {
				flags |= VMANIP_BLOCK_DATA_INEXIST;

				/*
					Mark area inexistent
				*/
				VoxelArea a(p*MAP_BLOCKSIZE, (p+1)*MAP_BLOCKSIZE-v3s16(1,1,1));
				// Fill with VOXELFLAG_NO_DATA
				for(s32 z=a.MinEdge.Z; z<=a.MaxEdge.Z; z++)
				for(s32 y=a.MinEdge.Y; y<=a.MaxEdge.Y; y++)
				{
					s32 i = m_area.index(a.MinEdge.X,y,z);
					memset(&m_flags[i], VOXELFLAG_NO_DATA, MAP_BLOCKSIZE);
				}
			}
		}
		/*else if (block->getNode(0, 0, 0).getContent() == CONTENT_IGNORE)
		{
			// Mark that block was loaded as blank
			flags |= VMANIP_BLOCK_CONTAINS_CIGNORE;
		}*/

		m_loaded_blocks[p] = flags;
	}

	m_is_dirty = false;
}

void MMVManip::blitBackAll(std::map<v3s16, MapBlock*> *modified_blocks,
	bool overwrite_generated)
{
	if(m_area.getExtent() == v3s16(0,0,0))
		return;

	/*
		Copy data of all blocks
	*/
	for(std::map<v3s16, u8>::iterator
			i = m_loaded_blocks.begin();
			i != m_loaded_blocks.end(); ++i)
	{
		v3s16 p = i->first;
		MapBlock *block = m_map->getBlockNoCreateNoEx(p);
		bool existed = !(i->second & VMANIP_BLOCK_DATA_INEXIST);
		if ((existed == false) || (block == NULL) ||
			(overwrite_generated == false && block->isGenerated() == true))
			continue;

		block->copyFrom(*this);

		if(modified_blocks)
			(*modified_blocks)[p] = block;
	}
}

//END
