#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hexdump.c"
#include "buffer.h"
#include "tux3.h"

unsigned freeblocks;

block_t balloc_range(struct inode *inode, block_t start, block_t count)
{
	block_t limit = start + count;
	unsigned blockbits = inode->map->dev->bits;
	unsigned blocksize = 1 << blockbits;
	unsigned mapshift = blockbits + 3;
	unsigned mapmask = (1 << mapshift) - 1;
	unsigned blocks = (limit + mapmask) >> mapshift;
	unsigned offset = (start & mapmask) >> 3;
	unsigned startbit = start & 7;
	block_t tail = (count + startbit + 7) >> 3;

	for (unsigned block = start >> mapshift; block < blocks; block++) {
		//printf("search block %x/%x\n", block, blocks);
		struct buffer *buffer = bread(inode->map, block);
		if (!buffer)
			return -1;
		unsigned bytes = blocksize - offset;
		if (bytes > tail)
			bytes = tail;
		unsigned char *p = buffer->data + offset, *top = p + bytes, c;
		for (; p < top; p++, startbit = 0) {
			if ((c = *p) == 0xff)
				continue;
			for (int i = startbit, mask = 1 << startbit; i < 8; i++, mask <<= 1) {
				if ((c & mask))
					continue;
				block_t found = i + (((void *)p - buffer->data) << 3) + (block << mapshift);
				if (found >= limit) {
					assert(block == blocks - 1);
					goto final_partial_byte;
				}
				set_bit(buffer->data, found & mapmask);
				freeblocks--;
				set_buffer_dirty(buffer);
				brelse(buffer);
				return found;
			}
		}
final_partial_byte:
		brelse(buffer);
		tail -= bytes;
		offset = 0;
	}
	return -1;
}

#if 0
block_t balloc(SB)
{
	block_t goal = sb->nextalloc, total = sb->image.blocks, block;
	if ((block = balloc_range(sb->bitmap, goal, total - goal)) >= 0)
		goto found;
	if ((block = balloc_range(sb->bitmap, 0, goal)) >= 0)
		goto found;
	return -1;
found:
	sb->nextalloc = block + 1;
	//set_sb_dirty(sb);
	return block;
}
#else
block_t balloc(SB)
{
	return sb->nextalloc++;
}
#endif

#ifndef main
int main(int argc, char *argv[])
{
	struct dev *dev = &(struct dev){ .bits = 3 };
	struct map *map = new_map(dev, NULL);
	struct inode *bitmap = &(struct inode){ .map = map };
	init_buffers(dev, 1 << 20);
	unsigned blocksize = 1 << dev->bits;
	unsigned dumpsize = blocksize > 16 ? 16 : blocksize;

	for (int block = 0; block < 10; block++) {
		struct buffer *buffer = getblk(map, block);
		memset(buffer->data, 0, blocksize);
		set_buffer_uptodate(buffer);
	}
	for (int i = 0; i < 12; i++) {
		block_t block = balloc_range(bitmap, 121, 10);
		printf("%Li\n", block);
	}
	hexdump(getblk(map, 0)->data, dumpsize);
	hexdump(getblk(map, 1)->data, dumpsize);
	hexdump(getblk(map, 2)->data, dumpsize);

	struct sb *sb = &(struct sb){ .image = { .blocks = 100 }, .nextalloc = 100, .bitmap = bitmap };

	for (int i = 0; i < 10; i++) {
		block_t block = balloc(sb);
		printf("0x%Lx\n", block);
	}
	hexdump(getblk(map, 0)->data, dumpsize);
	hexdump(getblk(map, 1)->data, dumpsize);
	hexdump(getblk(map, 2)->data, dumpsize);
	return 0;
}
#endif