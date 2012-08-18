/*
 * Tux3 versioning filesystem in user space
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include "tux3user.h"

#ifndef trace
#define trace trace_on
#endif

#include "kernel/super.c"

#ifdef ATOMIC
static void clean_dirty_buffer(const char *str, struct list_head *head)
{
	struct buffer_head *buf, *n;

	list_for_each_entry_safe(buf, n, head, link) {
		trace(">>> clean %s buffer %Lx:%Lx, count %d, state %d",
		      str, tux_inode(buffer_inode(buf))->inum,
		      bufindex(buf), bufcount(buf),
		      buf->state);
		assert(buffer_dirty(buf));
		set_buffer_clean(buf);
	}
}

static void clean_dirty_inode(const char *str, struct inode *inode)
{
	if (inode->i_state & I_DIRTY) {
		trace(">>> clean %s inode i_count %d, i_state %lx",
		      str, atomic_read(&inode->i_count), inode->i_state);
		del_defer_alloc_inum(inode);
		clear_inode(inode);
	}
}
#endif

static void cleanup_garbage_for_debugging(struct sb *sb)
{
#ifdef ATOMIC
	int rollup = sb->rollup;

	/*
	 * Pinned buffer is not flushing always, it is normal. So,
	 * this clean those for unmount to check buffer debugging
	 */
	if (sb->bitmap) {
		struct dirty_buffers *dirty = &mapping(sb->bitmap)->dirty;
		clean_dirty_buffer("bitmap", dirty_head_when(dirty, rollup));
		clean_dirty_inode("bitmap", sb->bitmap);
	}
	clean_dirty_buffer("pinned", dirty_head_when(&sb->pinned, rollup));

	/* orphan_add should be empty */
	assert(&sb->orphan_add);
	/* Deferred orphan deletion request is not flushed for each delta  */
	clean_orphan_list(&sb->orphan_del);

	/* defree must be flushed for each delta */
	assert(flink_empty(&sb->defree.head)||flink_is_last(&sb->defree.head));
#else /* !ATOMIC */
	/*
	 * Clean garbage (atomic commit) stuff. Don't forget to update
	 * this, if you update the atomic commit.
	 */
	log_finish(sb);
	log_finish_cycle(sb);

	if (sb->logmap)
		invalidate_buffers(sb->logmap->map);

	assert(flink_empty(&sb->defree.head)||flink_is_last(&sb->defree.head));
	assert(flink_empty(&sb->derollup.head));
	assert(list_empty(&sb->pinned));
#endif /* !ATOMIC */
}

int put_super(struct sb *sb)
{
	/*
	 * FIXME: Some test programs may not be loading inodes.
	 * All programs should load all internal inodes.
	 */

	cleanup_garbage_for_debugging(sb);

	__tux3_put_super(sb);

	inode_leak_check();

	return 0;
}

static int clear_other_magic(struct sb *sb)
{
	int err;

	/* Clear first and last block to get rid of other magic */
	for (int i = 0; i <= 1; i++) {
		loff_t loc = (loff_t[2]){ 0, (sb->volblocks - 1) << sb->blockbits }[i];
		unsigned len = (loff_t[2]){ SB_LOC, sb->blocksize }[i];
		char data[len];
		memset(data, 0, len);
		err = devio(WRITE, sb->dev, loc, data, len);
		if (err)
			break;
	}
	return err;
}

static int reserve_superblock(struct sb *sb)
{
	trace("reserve superblock");
	/* Always 8K regardless of blocksize */
	int reserve = 1 << (sb->blockbits > 13 ? 0 : 13 - sb->blockbits);
	for (int i = 0; i < reserve; i++) {
		block_t block = balloc_from_range(sb, i, 1, 1);
		if (block == -1)
			return -ENOSPC; // fix error code ???
		log_balloc(sb, block, 1);
		trace("reserve %Lx", block);
	}

	return 0;
}

int make_tux3(struct sb *sb)
{
	int err;

	err = clear_other_magic(sb);
	if (err)
		return err;

	trace("create bitmap");
	sb->bitmap = create_internal_inode(sb, TUX_BITMAP_INO, NULL);
	if (IS_ERR(sb->bitmap)) {
		err = PTR_ERR(sb->bitmap);
		goto eek;
	}

	if (reserve_superblock(sb) < 0)
		goto eek;

	trace("create version table");
	sb->vtable = create_internal_inode(sb, TUX_VTABLE_INO, NULL);
	if (IS_ERR(sb->vtable)) {
		err = PTR_ERR(sb->vtable);
		goto eek;
	}

	trace("create atom dictionary");
	sb->atable = create_internal_inode(sb, TUX_ATABLE_INO, NULL);
	if (IS_ERR(sb->atable)) {
		err = PTR_ERR(sb->atable);
		goto eek;
	}

	trace("create root directory");
	struct tux_iattr root_iattr = { .mode = S_IFDIR | 0755, };
	sb->rootdir = create_internal_inode(sb, TUX_ROOTDIR_INO, &root_iattr);
	if (IS_ERR(sb->rootdir)) {
		err = PTR_ERR(sb->rootdir);
		goto eek;
	}

	if ((err = sync_super(sb)))
		goto eek;

	show_buffers(mapping(sb->bitmap));
	show_buffers(mapping(sb->rootdir));
	show_buffers(sb->volmap->map);
	return 0;
eek:
	if (err)
		warn("eek, %s", strerror(-err));
	iput(sb->bitmap);
	sb->bitmap = NULL;
	return err ? err : -ENOSPC; // just guess
}
