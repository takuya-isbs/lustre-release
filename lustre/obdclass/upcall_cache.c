/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2012, 2016, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * lustre/obdclass/upcall_cache.c
 *
 * Supplementary groups cache.
 */
#define DEBUG_SUBSYSTEM S_SEC

#include <libcfs/libcfs.h>
#include <uapi/linux/lnet/lnet-types.h>
#include <upcall_cache.h>

static struct upcall_cache_entry *alloc_entry(struct upcall_cache *cache,
					      __u64 key, void *args)
{
	struct upcall_cache_entry *entry;

	LIBCFS_ALLOC(entry, sizeof(*entry));
	if (!entry)
		return NULL;

	UC_CACHE_SET_NEW(entry);
	INIT_LIST_HEAD(&entry->ue_hash);
	entry->ue_key = key;
	atomic_set(&entry->ue_refcount, 0);
	init_waitqueue_head(&entry->ue_waitq);
	entry->ue_acquire_expire = 0;
	entry->ue_expire = 0;
	if (cache->uc_ops->init_entry)
		cache->uc_ops->init_entry(entry, args);
	return entry;
}

/* protected by cache lock */
static void free_entry(struct upcall_cache *cache,
		       struct upcall_cache_entry *entry)
{
	if (cache->uc_ops->free_entry)
		cache->uc_ops->free_entry(cache, entry);

	list_del(&entry->ue_hash);
	CDEBUG(D_OTHER, "destroy cache entry %p for key %llu\n",
		entry, entry->ue_key);
	LIBCFS_FREE(entry, sizeof(*entry));
}

static inline int upcall_compare(struct upcall_cache *cache,
				 struct upcall_cache_entry *entry,
				 __u64 key, void *args)
{
	if (entry->ue_key != key)
		return -1;

	if (cache->uc_ops->upcall_compare)
		return cache->uc_ops->upcall_compare(cache, entry, key, args);

	return 0;
}

static inline int downcall_compare(struct upcall_cache *cache,
				   struct upcall_cache_entry *entry,
				   __u64 key, void *args)
{
	if (entry->ue_key != key)
		return -1;

	if (cache->uc_ops->downcall_compare)
		return cache->uc_ops->downcall_compare(cache, entry, key, args);

	return 0;
}

static inline void get_entry(struct upcall_cache_entry *entry)
{
	atomic_inc(&entry->ue_refcount);
}

static inline void put_entry(struct upcall_cache *cache,
			     struct upcall_cache_entry *entry)
{
	if (atomic_dec_and_test(&entry->ue_refcount) &&
	    (UC_CACHE_IS_INVALID(entry) || UC_CACHE_IS_EXPIRED(entry))) {
		free_entry(cache, entry);
	}
}

static int check_unlink_entry(struct upcall_cache *cache,
			      struct upcall_cache_entry *entry)
{
	time64_t now = ktime_get_seconds();

	if (UC_CACHE_IS_VALID(entry) && now < entry->ue_expire)
		return 0;

	if (UC_CACHE_IS_ACQUIRING(entry)) {
		if (entry->ue_acquire_expire == 0 ||
		    now < entry->ue_acquire_expire)
			return 0;

		UC_CACHE_SET_EXPIRED(entry);
		wake_up(&entry->ue_waitq);
	} else if (!UC_CACHE_IS_INVALID(entry)) {
		UC_CACHE_SET_EXPIRED(entry);
	}

	list_del_init(&entry->ue_hash);
	if (!atomic_read(&entry->ue_refcount))
		free_entry(cache, entry);
	return 1;
}

static inline int refresh_entry(struct upcall_cache *cache,
			 struct upcall_cache_entry *entry)
{
	LASSERT(cache->uc_ops->do_upcall);
	return cache->uc_ops->do_upcall(cache, entry);
}

struct upcall_cache_entry *upcall_cache_get_entry(struct upcall_cache *cache,
						  __u64 key, void *args)
{
	struct upcall_cache_entry *entry = NULL, *new = NULL, *next;
	bool failedacquiring = false;
	struct list_head *head;
	wait_queue_entry_t wait;
	int rc, found;
	ENTRY;

	LASSERT(cache);

	head = &cache->uc_hashtable[UC_CACHE_HASH_INDEX(key,
							cache->uc_hashsize)];
find_again:
	found = 0;
	spin_lock(&cache->uc_lock);
	list_for_each_entry_safe(entry, next, head, ue_hash) {
		/* check invalid & expired items */
		if (check_unlink_entry(cache, entry))
			continue;
		if (upcall_compare(cache, entry, key, args) == 0) {
			found = 1;
			break;
		}
	}

	if (!found) {
		if (!new) {
			spin_unlock(&cache->uc_lock);
			new = alloc_entry(cache, key, args);
			if (!new) {
				CERROR("fail to alloc entry\n");
				RETURN(ERR_PTR(-ENOMEM));
			}
			goto find_again;
		} else {
			list_add(&new->ue_hash, head);
			entry = new;
		}
	} else {
		if (new) {
			free_entry(cache, new);
			new = NULL;
		}
		list_move(&entry->ue_hash, head);
	}
	get_entry(entry);

	/* acquire for new one */
	if (UC_CACHE_IS_NEW(entry)) {
		UC_CACHE_SET_ACQUIRING(entry);
		UC_CACHE_CLEAR_NEW(entry);
		spin_unlock(&cache->uc_lock);
		rc = refresh_entry(cache, entry);
		spin_lock(&cache->uc_lock);
		entry->ue_acquire_expire = ktime_get_seconds() +
					   cache->uc_acquire_expire;
		if (rc < 0) {
			UC_CACHE_CLEAR_ACQUIRING(entry);
			UC_CACHE_SET_INVALID(entry);
			wake_up(&entry->ue_waitq);
			if (unlikely(rc == -EREMCHG)) {
				put_entry(cache, entry);
				GOTO(out, entry = ERR_PTR(rc));
			}
		}
	}
	/* someone (and only one) is doing upcall upon this item,
	 * wait it to complete */
	if (UC_CACHE_IS_ACQUIRING(entry)) {
		long expiry = (entry == new) ?
			      cfs_time_seconds(cache->uc_acquire_expire) :
			      MAX_SCHEDULE_TIMEOUT;
		long left;

		init_wait(&wait);
		add_wait_queue(&entry->ue_waitq, &wait);
		set_current_state(TASK_INTERRUPTIBLE);
		spin_unlock(&cache->uc_lock);

		left = schedule_timeout(expiry);

		spin_lock(&cache->uc_lock);
		remove_wait_queue(&entry->ue_waitq, &wait);
		if (UC_CACHE_IS_ACQUIRING(entry)) {
			/* we're interrupted or upcall failed in the middle */
			rc = left > 0 ? -EINTR : -ETIMEDOUT;
			/* if we waited uc_acquire_expire, we can try again
			 * with same data, but only if acquire is replayable
			 */
			if (left <= 0 && !cache->uc_acquire_replay)
				failedacquiring = true;
			put_entry(cache, entry);
			if (!failedacquiring) {
				spin_unlock(&cache->uc_lock);
				failedacquiring = true;
				new = NULL;
				CDEBUG(D_OTHER,
				       "retry acquire for key %llu (got %d)\n",
				       entry->ue_key, rc);
				goto find_again;
			}
			CERROR("acquire for key %llu: error %d\n",
			       entry->ue_key, rc);
			GOTO(out, entry = ERR_PTR(rc));
		}
	}

	/* invalid means error, don't need to try again */
	if (UC_CACHE_IS_INVALID(entry)) {
		put_entry(cache, entry);
		GOTO(out, entry = ERR_PTR(-EIDRM));
	}

	/* check expired
	 * We can't refresh the existing one because some
	 * memory might be shared by multiple processes.
	 */
	if (check_unlink_entry(cache, entry)) {
		/* if expired, try again. but if this entry is
		 * created by me but too quickly turn to expired
		 * without any error, should at least give a
		 * chance to use it once.
		 */
		if (entry != new) {
			put_entry(cache, entry);
			spin_unlock(&cache->uc_lock);
			new = NULL;
			goto find_again;
		}
	}

	/* Now we know it's good */
out:
	spin_unlock(&cache->uc_lock);
	RETURN(entry);
}
EXPORT_SYMBOL(upcall_cache_get_entry);

void upcall_cache_get_entry_raw(struct upcall_cache_entry *entry)
{
	get_entry(entry);
}
EXPORT_SYMBOL(upcall_cache_get_entry_raw);

void upcall_cache_update_entry(struct upcall_cache *cache,
			       struct upcall_cache_entry *entry,
			       time64_t expire, int state)
{
	spin_lock(&cache->uc_lock);
	entry->ue_expire = expire;
	if (!state)
		UC_CACHE_SET_VALID(entry);
	else
		entry->ue_flags |= state;
	spin_unlock(&cache->uc_lock);
}
EXPORT_SYMBOL(upcall_cache_update_entry);

void upcall_cache_put_entry(struct upcall_cache *cache,
			    struct upcall_cache_entry *entry)
{
	ENTRY;

	if (!entry) {
		EXIT;
		return;
	}

	LASSERT(atomic_read(&entry->ue_refcount) > 0);
	spin_lock(&cache->uc_lock);
	put_entry(cache, entry);
	spin_unlock(&cache->uc_lock);
	EXIT;
}
EXPORT_SYMBOL(upcall_cache_put_entry);

int upcall_cache_downcall(struct upcall_cache *cache, __u32 err, __u64 key,
			  void *args)
{
	struct upcall_cache_entry *entry = NULL;
	struct list_head *head;
	int found = 0, rc = 0;
	ENTRY;

	LASSERT(cache);

	head = &cache->uc_hashtable[UC_CACHE_HASH_INDEX(key,
							cache->uc_hashsize)];

	spin_lock(&cache->uc_lock);
	list_for_each_entry(entry, head, ue_hash) {
		if (downcall_compare(cache, entry, key, args) == 0) {
			found = 1;
			get_entry(entry);
			break;
		}
	}

	if (!found) {
		CDEBUG(D_OTHER, "%s: upcall for key %llu not expected\n",
		       cache->uc_name, key);
		/* haven't found, it's possible */
		spin_unlock(&cache->uc_lock);
		RETURN(-EINVAL);
	}

	if (err) {
		CDEBUG(D_OTHER, "%s: upcall for key %llu returned %d\n",
		       cache->uc_name, entry->ue_key, err);
		GOTO(out, rc = err);
	}

	if (!UC_CACHE_IS_ACQUIRING(entry)) {
		CDEBUG(D_RPCTRACE, "%s: found uptodate entry %p (key %llu)"
		       "\n", cache->uc_name, entry, entry->ue_key);
		GOTO(out, rc = 0);
	}

	if (UC_CACHE_IS_INVALID(entry) || UC_CACHE_IS_EXPIRED(entry)) {
		CERROR("%s: found a stale entry %p (key %llu) in ioctl\n",
		       cache->uc_name, entry, entry->ue_key);
		GOTO(out, rc = -EINVAL);
	}

	spin_unlock(&cache->uc_lock);
	if (cache->uc_ops->parse_downcall)
		rc = cache->uc_ops->parse_downcall(cache, entry, args);
	spin_lock(&cache->uc_lock);
	if (rc)
		GOTO(out, rc);

	if (!entry->ue_expire)
		entry->ue_expire = ktime_get_seconds() + cache->uc_entry_expire;
	UC_CACHE_SET_VALID(entry);
	CDEBUG(D_OTHER, "%s: created upcall cache entry %p for key %llu\n",
	       cache->uc_name, entry, entry->ue_key);
out:
	if (rc) {
		UC_CACHE_SET_INVALID(entry);
		list_del_init(&entry->ue_hash);
	}
	UC_CACHE_CLEAR_ACQUIRING(entry);
	spin_unlock(&cache->uc_lock);
	wake_up(&entry->ue_waitq);
	put_entry(cache, entry);

	RETURN(rc);
}
EXPORT_SYMBOL(upcall_cache_downcall);

void upcall_cache_flush(struct upcall_cache *cache, int force)
{
	struct upcall_cache_entry *entry, *next;
	int i;
	ENTRY;

	spin_lock(&cache->uc_lock);
	for (i = 0; i < cache->uc_hashsize; i++) {
		list_for_each_entry_safe(entry, next,
					 &cache->uc_hashtable[i], ue_hash) {
			if (!force && atomic_read(&entry->ue_refcount)) {
				UC_CACHE_SET_EXPIRED(entry);
				continue;
			}
			LASSERT(!atomic_read(&entry->ue_refcount));
			free_entry(cache, entry);
		}
	}
	spin_unlock(&cache->uc_lock);
	EXIT;
}
EXPORT_SYMBOL(upcall_cache_flush);

void upcall_cache_flush_one(struct upcall_cache *cache, __u64 key, void *args)
{
	struct list_head *head;
	struct upcall_cache_entry *entry;
	int found = 0;
	ENTRY;

	head = &cache->uc_hashtable[UC_CACHE_HASH_INDEX(key,
							cache->uc_hashsize)];

	spin_lock(&cache->uc_lock);
	list_for_each_entry(entry, head, ue_hash) {
		if (upcall_compare(cache, entry, key, args) == 0) {
			found = 1;
			break;
		}
	}

	if (found) {
		CWARN("%s: flush entry %p: key %llu, ref %d, fl %x, "
		      "cur %lld, ex %lld/%lld\n",
		      cache->uc_name, entry, entry->ue_key,
		      atomic_read(&entry->ue_refcount), entry->ue_flags,
		      ktime_get_real_seconds(), entry->ue_acquire_expire,
		      entry->ue_expire);
		UC_CACHE_SET_EXPIRED(entry);
		if (!atomic_read(&entry->ue_refcount))
			free_entry(cache, entry);
	}
	spin_unlock(&cache->uc_lock);
}
EXPORT_SYMBOL(upcall_cache_flush_one);

struct upcall_cache *upcall_cache_init(const char *name, const char *upcall,
				       int hashsz, time64_t entry_expire,
				       time64_t acquire_expire, bool replayable,
				       struct upcall_cache_ops *ops)
{
	struct upcall_cache *cache;
	int i;
	ENTRY;

	LIBCFS_ALLOC(cache, sizeof(*cache));
	if (!cache)
		RETURN(ERR_PTR(-ENOMEM));

	spin_lock_init(&cache->uc_lock);
	init_rwsem(&cache->uc_upcall_rwsem);
	cache->uc_hashsize = hashsz;
	LIBCFS_ALLOC(cache->uc_hashtable,
		     sizeof(*cache->uc_hashtable) * cache->uc_hashsize);
	if (!cache->uc_hashtable)
		RETURN(ERR_PTR(-ENOMEM));
	for (i = 0; i < cache->uc_hashsize; i++)
		INIT_LIST_HEAD(&cache->uc_hashtable[i]);
	strlcpy(cache->uc_name, name, sizeof(cache->uc_name));
	/* upcall pathname proc tunable */
	strlcpy(cache->uc_upcall, upcall, sizeof(cache->uc_upcall));
	cache->uc_entry_expire = entry_expire;
	cache->uc_acquire_expire = acquire_expire;
	cache->uc_acquire_replay = replayable;
	cache->uc_ops = ops;

	RETURN(cache);
}
EXPORT_SYMBOL(upcall_cache_init);

void upcall_cache_cleanup(struct upcall_cache *cache)
{
	if (!cache)
		return;
	upcall_cache_flush_all(cache);
	LIBCFS_FREE(cache->uc_hashtable,
		    sizeof(*cache->uc_hashtable) * cache->uc_hashsize);
	LIBCFS_FREE(cache, sizeof(*cache));
}
EXPORT_SYMBOL(upcall_cache_cleanup);
