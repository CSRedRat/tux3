/*
 * Copied some *_write_end() to replace mark_buffer_dirty() of
 * write_end() by tux3_mark_buffer_dirty().
 *
 * We should check the update of original functions, and sync with it.
 */

/* Copy of __block_commit_write() (changed to call tux3_mark_buffer_dirty()) */
static int __tux3_commit_write(struct inode *inode, struct page *page,
			       unsigned from, unsigned to)
{
	unsigned block_start, block_end;
	int partial = 0;
	unsigned blocksize;
	struct buffer_head *bh, *head;

	blocksize = 1 << inode->i_blkbits;

	for(bh = head = page_buffers(page), block_start = 0;
	    bh != head || !block_start;
	    block_start=block_end, bh = bh->b_this_page) {
		block_end = block_start + blocksize;
		if (block_end <= from || block_start >= to) {
			if (!buffer_uptodate(bh))
				partial = 1;
		} else {
			set_buffer_uptodate(bh);
			tux3_mark_buffer_dirty(bh);
		}
		clear_buffer_new(bh);
	}

	/*
	 * If this is a partial write which happened to make all buffers
	 * uptodate then we can optimize away a bogus readpage() for
	 * the next read(). Here we 'discover' whether the page went
	 * uptodate as a result of this (potentially partial) write.
	 */
	if (!partial)
		SetPageUptodate(page);
	return 0;
}

/* Copy of block_write_end() */
static int __tux3_write_end(struct file *file, struct address_space *mapping,
			    loff_t pos, unsigned len, unsigned copied,
			    struct page *page, void *fsdata)
{
	struct inode *inode = mapping->host;
	unsigned start;

	start = pos & (PAGE_CACHE_SIZE - 1);

	if (unlikely(copied < len)) {
		/*
		 * The buffers that were written will now be uptodate, so we
		 * don't have to worry about a readpage reading them and
		 * overwriting a partial write. However if we have encountered
		 * a short write and only partially written into a buffer, it
		 * will not be marked uptodate, so a readpage might come in and
		 * destroy our partial write.
		 *
		 * Do the simplest thing, and just treat any short write to a
		 * non uptodate page as a zero-length write, and force the
		 * caller to redo the whole thing.
		 */
		if (!PageUptodate(page))
			copied = 0;

		page_zero_new_buffers(page, start+copied, start+len);
	}
	flush_dcache_page(page);

	/* This could be a short (even 0-length) commit */
	__tux3_commit_write(inode, page, start, start+copied);

	return copied;
}

/* Copy of generic_write_end() */
static int tux3_write_end(struct file *file, struct address_space *mapping,
			  loff_t pos, unsigned len, unsigned copied,
			  struct page *page, void *fsdata)
{
	struct inode *inode = mapping->host;
	int i_size_changed = 0;

	copied = __tux3_write_end(file, mapping, pos, len, copied, page, fsdata);

	/*
	 * No need to use i_size_read() here, the i_size
	 * cannot change under us because we hold i_mutex.
	 *
	 * But it's important to update i_size while still holding page lock:
	 * page writeout could otherwise come in and zero beyond i_size.
	 */
	if (pos+copied > inode->i_size) {
		i_size_write(inode, pos+copied);
		i_size_changed = 1;
	}

	unlock_page(page);
	page_cache_release(page);

	/*
	 * Don't mark the inode dirty under page lock. First, it unnecessarily
	 * makes the holding time of page lock longer. Second, it forces lock
	 * ordering of page lock and transaction start for journaling
	 * filesystems.
	 */
	if (i_size_changed)
		mark_inode_dirty(inode);

	return copied;
}
