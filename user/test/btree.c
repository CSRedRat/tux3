/*
 * Generic btree operations
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Portions copyright (c) 2006-2008 Google Inc.
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include <stdlib.h>
#include <stddef.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include "diskio.h"
#include "buffer.h"
#include "tux3.h"

#define main notmain
#include "fleaf.c"
#undef main
#define main notmain2
#include "ileaf.c"
#undef main

#define trace trace_on

typedef struct fleaf leaf_t;
typedef u32 millisecond_t;

static millisecond_t gettime(void)
{
	struct timeval now;
	if (gettimeofday(&now, NULL) != -1)
		return now.tv_sec * 1000LL + now.tv_usec / 1000;
	error("gettimeofday failed, %s (%i)", strerror(errno), errno);
	return 0;
}

struct btree_ops {
	int (*leaf_sniff)(SB, void *leaf);
	int (*leaf_init)(SB, void *leaf);
	tuxkey_t (*leaf_split)(SB, void *base, void *base2, int fudge);
	void *(*leaf_expand)(SB, void *base, inum_t inum, unsigned more);
};

struct btree_ops ftree_ops = {
	.leaf_sniff = /*(typeof(fieldtype(btree_ops, leaf_sniff)))*/leaf_sniff,
	.leaf_init = leaf_init,
	.leaf_split = leaf_split,
	.leaf_expand = leaf_expand,
};

struct btree_ops itree_ops = {
	.leaf_sniff = ileaf_sniff,
	.leaf_init = ileaf_init,
	.leaf_split = ileaf_split,
	.leaf_expand = ileaf_expand,
};

struct bnode
{
	u32 count, unused;
	struct index_entry { u64 key; block_t block; } entries[];
}; 
/*
 * Note that the first key of an index block is never accessed.  This is
 * because for a btree, there is always one more key than nodes in each
 * index node.  In other words, keys lie between node pointers.  I
 * micro-optimize by placing the node count in the first key, which allows
 * a node to contain an esthetically pleasing binary number of pointers.
 * (Not done yet.)
 */

static block_t balloc(SB)
{
        return ++sb->image.last_alloc;
}

static void free_block(SB, sector_t block)
{
}

static struct buffer *new_block(SB)
{
        return getblk(sb->dev, balloc(sb));
}

static struct buffer *new_leaf(SB, struct btree_ops *ops)
{
	trace(printf("new leaf blocksize = %i\n", sb->blocksize););
	struct buffer *buffer = new_block(sb); 
	if (!buffer)
		return NULL;
	memset(buffer->data, 0, bufsize(buffer));
	(ops->leaf_init)(sb, buffer->data);
	set_buffer_dirty(buffer);
	return buffer;
}

static struct buffer *new_node(SB)
{
	trace(printf("new node\n"););
	struct buffer *buffer = new_block(sb); 
	if (!buffer)
		return buffer;
	memset(buffer->data, 0, bufsize(buffer));
	struct bnode *node = buffer->data;
	node->count = 0;
	set_buffer_dirty(buffer);
	return buffer;
}

static struct buffer *blockread(SB, block_t block)
{
	return bread(sb->dev, block);
}

struct treepath { struct buffer *buffer; struct index_entry *next; };
struct leafpath { struct buffer *buffer; void *object; };

/*
 * A btree path for a btree of depth N consists of N treepath entries
 * and one pathleaf entry.  After a probe the treepath->next fields
 * point at the next entry that will be accessed if the path is
 * advanced, which seemed to work out better than pointing right at
 * the element accessed, particularly for mass delete.  On the other
 * hand, the leaf object points right at the object.
 */

static inline struct bnode *path_node(struct treepath path[], int level)
{
	return (struct bnode *)path[level].buffer;
}

static void brelse_path(struct treepath *path, unsigned levels)
{
	unsigned i;
	for (i = 0; i <= levels; i++)
{
printf("release buffer for %Li\n", path[i].buffer->block);
		brelse(path[i].buffer);
}
}

static int probe(SB, tuxkey_t target, struct treepath *path, struct btree_ops *ops)
{
	unsigned i, levels = sb->image.levels;
	struct buffer *buffer = blockread(sb, sb->image.root);
	if (!buffer)
		goto eek;
	struct bnode *node = buffer->data;

	for (i = 0; i < levels; i++) {
		struct index_entry *next = node->entries, *top = next + node->count;
		while (++next < top) /* binary search goes here */
			if (next->key > target)
				break;
		path[i] = (struct treepath){ buffer, next };
		if (!(buffer = blockread(sb, (next - 1)->block)))
			goto eek;
		node = (struct bnode *)buffer->data;
	}
	assert((ops->leaf_sniff)(sb, buffer->data));
	path[levels] = (struct treepath){ buffer };
	return 0;
eek:
	brelse_path(path, i);
	return -EIO; /* stupid, it might have been NOMEM */
}

static void show_leaf_range(SB, struct fleaf *leaf, block_t start, block_t finish)
{
	leaf_dump(sb, leaf);
}

static void show_subtree_range(SB, struct bnode *node, block_t start, block_t finish, int levels, int indent)
{
	int i;

	for (i = 0; i < node->count; i++) {
		struct buffer *buffer = blockread(sb, node->entries[i].block);
		if (levels)
			show_subtree_range(sb, buffer->data, start, finish, levels - 1, indent + 3);
		else {
			show_leaf_range(sb, buffer->data, start, finish);
		}
		brelse(buffer);
	}
}

void show_tree_range(SB, block_t start, block_t finish)
{
	struct buffer *buffer = blockread(sb, sb->image.root);
	if (!buffer)
		return;
	show_subtree_range(sb, buffer->data, start, finish, sb->image.levels - 1, 0);
	brelse(buffer);
}

void show_tree(SB)
{
	show_tree_range(sb, 0, -1);
}

static void remove_index(struct treepath path[], int level)
{
	struct bnode *node = path_node(path, level);
	int count = node->count, i;

	// stomps the node count (if 0th key holds count)
	memmove(path[level].next - 1, path[level].next,
		(char *)&node->entries[count] - (char *)path[level].next);
	node->count = count - 1;
	--(path[level].next);
	set_buffer_dirty(path[level].buffer);

	// no pivot for last entry
	if (path[level].next == node->entries + node->count)
		return;

	// climb up to common parent and set pivot to deleted key
	// what if index is now empty? (no deleted key)
	// then some key above is going to be deleted and used to set pivot
	if (path[level].next == node->entries && level) {
		block_t pivot = (path[level].next)->key;
		/* climb the path while at first entry */
		for (i = level - 1; path[i].next - 1 == path_node(path, i)->entries; i--)
			if (!i) /* bail out at root */
				return;
		/* found the node with the old pivot, set it to deleted key */
		(path[i].next - 1)->key = pivot;
		set_buffer_dirty(path[i].buffer);
	}
}

static void merge_nodes(struct bnode *node, struct bnode *node2)
{
	memcpy(&node->entries[node->count], &node2->entries[0], node2->count * sizeof(struct index_entry));
	node->count += node2->count;
}

static void brelse_free(SB, struct buffer *buffer)
{
	brelse(buffer);
	if (buffer->count) {
		warn("free block %Lx still in use!", (long long)buffer->block);
		return;
	}
	free_block(sb, buffer->block);
	set_buffer_empty(buffer);
}

static inline int finished_level(struct treepath path[], int level)
{
	struct bnode *node = path_node(path, level);
	return path[level].next == node->entries + node->count;
}

typedef u32 tag_t;

struct delete_info { tag_t victim, newtag; block_t blocks; block_t resume; int create; };

int delete_snapshots_from_leaf(SB, leaf_t *leaf, struct delete_info *info, int *freed)
{
	return 0;
}

int delete_tree_partial(SB, struct delete_info *info, millisecond_t deadline, int maxblocks)
{
	int levels = sb->image.levels, level = levels - 1, suspend = 0, freed = 0;
	struct treepath path[levels + 1], prev[levels + 1];
	struct buffer *leafbuf, *leafprev = NULL;
	memset(path, 0, sizeof(path));

	probe(sb, info->resume, path, &ftree_ops);
	leafbuf = path[levels].buffer;

	/* leaf walk */
	while (1) {
		if (delete_snapshots_from_leaf(sb, leafbuf->data, info, &freed))
			set_buffer_dirty(leafbuf);

		/* try to merge this leaf with prev */
		if (leafprev) {
			leaf_t *this = leafbuf->data;
			leaf_t *that = leafprev->data;
			trace_off(warn("check leaf %p against %p", leafbuf, leafprev););
			trace_off(warn("need = %i, free = %i", leaf_used(sb, this), leaf_free(sb, that)););
			/* try to merge leaf with prev */
			if (leaf_used(sb, this) <= leaf_free(sb, that)) {
				trace_off(warn(">>> can merge leaf %p into leaf %p", leafbuf, leafprev););
				leaf_merge(sb, that, this);
				remove_index(path, level);
				set_buffer_dirty(leafprev);
				brelse_free(sb, leafbuf);
				//dirty_buffer_count_check(sb);
				goto keep_prev_leaf;
			}
			brelse(leafprev);
		}
		leafprev = leafbuf;
keep_prev_leaf:

		//nanosleep(&(struct timespec){ 0, 50 * 1000000 }, NULL);
		//printf("time remaining: %Li\n", deadline - gettime());
		if (deadline != -1 && gettime() > deadline)
			suspend = -1;
		if (info->blocks != -1 && freed >= info->blocks)
			suspend = -1;

		/* pop and try to merge finished nodes */
		while (suspend || finished_level(path, level)) {
			/* try to merge node with prev */
			if (prev[level].buffer) {
				assert(level); /* root has no prev */
				struct bnode *this = path_node(path, level);
				struct bnode *that = path_node(prev, level);
				trace_off(warn("check node %p against %p", this, that););
				trace_off(warn("this count = %i prev count = %i", this->count, that->count););
				/* try to merge with node to left */
				if (this->count <= sb->alloc_per_node - that->count) {
					trace(warn(">>> can merge node %p into node %p", this, that););
					merge_nodes(that, this);
					remove_index(path, level - 1);
					set_buffer_dirty(prev[level].buffer);
					brelse_free(sb, path[level].buffer);
					//dirty_buffer_count_check(sb);
					goto keep_prev_node;
				}
				brelse(prev[level].buffer);
			}
			prev[level].buffer = path[level].buffer;
keep_prev_node:

			/* deepest key in the path is the resume address */
			if (suspend == -1 && !finished_level(path, level)) {
				suspend = 1; /* only set resume once */
				info->resume = (path[level].next)->key;
			}
			if (!level) { /* remove levels if possible */
				while (levels > 1 && path_node(prev, 0)->count == 1) {
					trace_off(warn("drop btree level"););
					sb->image.root = prev[1].buffer->block;
					brelse_free(sb, prev[0].buffer);
					//dirty_buffer_count_check(sb);
					levels = --sb->image.levels;
					memcpy(prev, prev + 1, levels * sizeof(prev[0]));
					//set_sb_dirty(sb);
				}
				brelse(leafprev);
				brelse_path(prev, levels);
				//sb->snapmask &= ~snapmask; delete_snapshot_from_disk();
				//set_sb_dirty(sb);
				//save_sb(sb);
				return suspend;
			}

			level--;
			trace_off(printf("pop to level %i, %i of %i nodes\n", level, path[level].next - path_node(path, level)->entries, path_node(path, level)->count););
		}

		/* push back down to leaf level */
		while (level < levels - 1) {
			struct buffer *buffer = blockread(sb, path[level++].next++->block);
			if (!buffer) {
				brelse(leafprev);
				brelse_path(path, level - 1);
				return -ENOMEM;
			}
			path[level].buffer = buffer;
			path[level].next = buffer->data;
			trace_off(printf("push to level %i, %i nodes\n", level, path_node(path, level)->count););
		};
		//dirty_buffer_count_check(sb);

		/* go to next leaf */
		if (!(leafbuf = blockread(sb, path[level].next++->block))) {
			brelse_path(path, level);
			return -ENOMEM;
		}
	}
}

static void insert_child(struct bnode *node, struct index_entry *p, block_t child, u64 childkey)
{
	memmove(p + 1, p, (char *)(&node->entries[0] + node->count) - (char *)p);
	p->block = child;
	p->key = childkey;
	node->count++;
}

static void *tree_expand(SB, u64 target, unsigned size, struct treepath path[], unsigned levels, struct btree_ops *ops)
{
	struct buffer *leafbuf = path[levels].buffer;
	set_buffer_dirty(leafbuf);
	void *entity = (ops->leaf_expand)(sb, leafbuf->data, target, size);
	if (entity)
		return entity;

	trace(warn("split leaf");)
	struct buffer *childbuf = new_leaf(sb, ops);
	if (!childbuf) 
		return NULL; // !!! err_ptr(ENOMEM) this is the right thing to do???
	u64 childkey = (ops->leaf_split)(sb, leafbuf->data, childbuf->data, 0);
	block_t childblock = childbuf->block;
	if (target < childkey) {
		struct buffer *swapbuf = leafbuf;
		leafbuf = childbuf;
		childbuf = swapbuf;
	}
	brelse_dirty(childbuf);
	entity = (ops->leaf_expand)(sb, leafbuf->data, target, size);
	assert(entity);

	while (levels--) {
		struct index_entry *next = path[levels].next;
		struct buffer *parentbuf = path[levels].buffer;
		struct bnode *parent = parentbuf->data;

		/* insert here if not full */
		if (parent->count < sb->alloc_per_node) {
			insert_child(parent, next, childblock, childkey);
			set_buffer_dirty(parentbuf);
			return entity;
		}

		/* split a full index node */
		unsigned half = parent->count / 2;
		u64 newkey = parent->entries[half].key;
		struct buffer *newbuf = new_node(sb); 
		if (!newbuf) 
			return NULL; // !!! err_ptr(ENOMEM)
		struct bnode *newnode = newbuf->data;
		newnode->count = parent->count - half;
		memcpy(&newnode->entries[0], &parent->entries[half], newnode->count * sizeof(struct index_entry));
		parent->count = half;

		/* If the path is in the new node, use that as the parent */
		if (next > parent->entries + half) {
			next = next - &parent->entries[half] + newnode->entries;
			set_buffer_dirty(parentbuf);
			parentbuf = newbuf;
			parent = newnode;
		} else set_buffer_dirty(newbuf);
		insert_child(parent, next, childblock, childkey);
		set_buffer_dirty(parentbuf);
		childkey = newkey;
		childblock = newbuf->block;
		brelse(newbuf);
	}
	trace(printf("add tree level\n");)
	struct buffer *newbuf = new_node(sb);
	if (!newbuf)
		return NULL; // !!! err_ptr(ENOMEM)
	struct bnode *newroot = newbuf->data;

	newroot->count = 2;
	newroot->entries[0].block = sb->image.root;
	newroot->entries[1].key = childkey;
	newroot->entries[1].block = childblock;
	sb->image.root = newbuf->block;
	sb->image.levels++;
	//set_sb_dirty(sb);
	brelse_dirty(newbuf);
	return entity;
}

static int tuxwrite(SB, block_t target, char *data, unsigned len)
{
	int err, levels = sb->image.levels;
	struct treepath path[levels + 1];
	if ((err = probe(sb, target, path, &ftree_ops)))
		return err;
	struct buffer *leafbuf = path[levels].buffer, *blockbuf;
	
	unsigned extents = 0;
	struct extent *found = leaf_lookup(sb, leafbuf->data, target, &extents);
	leaf_dump(sb, leafbuf->data);
	brelse_path(path, levels);

	if (extents) {
		trace(warn("found block %Lx", (long long)found->block);)
		if (!(blockbuf = blockread(sb, found->block)))
			return -EBADF;
	} else {
		blockbuf = new_block(sb);
		trace(warn("new block %Lx", blockbuf->block);)
		struct extent extent = { .block = blockbuf->block };
		struct extent *store = tree_expand(sb, target, sizeof(extent), path, levels, &itree_ops);
		*store = extent;
	}
	memcpy(blockbuf->data, data, len);
	set_buffer_dirty(blockbuf);
out:
	brelse(blockbuf);
	return err;
eek:
	warn("unable to add extent to tree: %s", strerror(-err));
	free_block(sb, blockbuf->block);
	goto out;
}

static int tuxread(SB, block_t seek, char *data, unsigned len)
{
	int err, levels = sb->image.levels;
	struct treepath path[levels + 1];
	if ((err = probe(sb, seek, path, &ftree_ops)))
		return err;
	struct buffer *leafbuf = path[levels].buffer, *blockbuf;
	
	unsigned count = 0;
	struct extent *found = leaf_lookup(sb, leafbuf->data, seek, &count);
	leaf_dump(sb, leafbuf->data);
	brelse_path(path, levels);

	if (count) {
		trace(warn("found block %Lx", (long long)found->block);)
		if (!(blockbuf = blockread(sb, found->block)))
			return -EBADF;
		memcpy(data, blockbuf->data, len);
		brelse(blockbuf);
	} else {
		memset(data, 0, len);
	}
	return 0;
}

enum { MTIME_SIZE };

struct size_mtime_attr { u64 kind:4, size:60, version:10, mtime:54; };

int tuxopen(SB, inum_t inum, char *attr, unsigned len, int create)
{
	int err, levels = sb->image.levels;
	struct treepath path[levels + 1];
	if ((err = probe(sb, inum, path, &itree_ops)))
		return err;
	struct buffer *leafbuf = path[levels].buffer, *blockbuf;
	
	unsigned size = 0;
	struct size_mtime_attr *found = ileaf_lookup(sb, leafbuf->data, inum, &size);
	ileaf_dump(sb, leafbuf->data);
	brelse_path(path, levels);

	if (size) {
		trace(warn("found inode %Lx", inum);)
		hexdump(found, size);
	} else {
		trace(warn("no inode %Lx", inum);)
		if (!create)
			return -ENOENT;
		trace(warn("new inode %Lx", inum);)
		struct size_mtime_attr attr = { .kind = MTIME_SIZE };
		typeof(attr) *store = tree_expand(sb, inum, sizeof(attr), path, levels, &itree_ops);
		*store = attr;
	}
	return 0;
eek:
	warn("unable to add inode to inode table: %s", strerror(-err));
	return err;
}

void init_tux3(SB)
{
	sb->blocksize = 1 << sb->dev->bits;
	sb->image.blockbits = sb->dev->bits;
	struct buffer *rootbuf = new_node(sb);
	struct buffer *leafbuf = new_leaf(sb, &itree_ops);
	sb->image.root = rootbuf->block;
	((struct bnode *)rootbuf->data)->count = 1;
	((struct bnode *)rootbuf->data)->entries[0].block = leafbuf->block;
	sb->image.root = rootbuf->block;
	sb->image.levels = 1;
	printf("root at %Li\n", rootbuf->block);
	printf("leaf at %Li\n", leafbuf->block);
	brelse_dirty(rootbuf);
	brelse_dirty(leafbuf);
}

int main(int argc, char *argv[])
{
	struct dev dev = { .fd = open(argv[1], O_CREAT|O_TRUNC|O_RDWR, S_IRWXU), .bits = 12 };
	struct sb sb = { .image = { .magic = SB_MAGIC }, .dev = &dev, .alloc_per_node = 20 };
	init_buffers(1 << dev.bits, 1 << 20);
	init_tux3(&sb);
	char buf[100] = { };

	tuxopen(&sb, 0x123, buf, sizeof(buf), 1); return 0;

	tuxwrite(&sb, 6, "hello world", 11);
	flush_buffers();

	if (tuxread(&sb, 6, buf, 11))
		return 1;
		
	hexdump(buf, 11);
	return 0;
}
