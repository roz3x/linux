#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include "ext4.h"

struct ext4_range_node {
	struct rb_node rb;

	int inode;
	u64 offset;
	enum {BOUNDRY_START, BOUNDRY_END} value;
	int refs;
	struct mutex internal_mutex; // TODO: init the mutex ??
};


static struct rb_root root = RB_ROOT;
static struct mutex lock;

static struct ext4_range_node* find_first_le(int inode, u64 offset) {

	struct rb_node **new = &root.rb_node;
	struct rb_node *parent = NULL;
	struct ext4_range_node* best = NULL;

	while (*new) {
		struct ext4_range_node *this = rb_entry(*new, struct ext4_range_node, rb);
		parent = *new;

		if (inode == this->inode) {
			if (offset == this->offset) {
				return this;
			} else if (offset < this->inode) {
				best = this;
				new = &((*new)->rb_left);
			} else {
				new = &((*new)->rb_right);
			}
		} else {
			if (inode < this->inode) {
				best = this;
				new = &((*new)->rb_left);
			} else {
				new = &((*new)->rb_right);
			}
		}
	}
	best->refs++;
	return best;
}

static void delete_exact(int inode, u64 offset) {

	struct rb_node **new = &root.rb_node;
	struct rb_node *parent = NULL;

	while (*new) {
		struct ext4_range_node *this = rb_entry(*new, struct ext4_range_node, rb);
		parent = *new;

		if (inode == this->inode) {
			if (offset == this->offset) {
				rb_erase(&this->rb, &root);
				kfree(this);
				return;
			} else if (offset < this->inode) {
				new = &((*new)->rb_left);
			} else {
				new = &((*new)->rb_right);
			}
		} else {
			if (inode < this->inode) {
				new = &((*new)->rb_left);
			} else {
				new = &((*new)->rb_right);
			}
		}
	}
	return;
}

void ext4_completion_cb(int inode, u64 start_offset, u64 end_offset) {

	mutex_lock(&lock);

	delete_exact(inode, start_offset);
	delete_exact(inode, start_offset);

	mutex_unlock(&lock);
}

void ext4_insert_or_wait(int inode, u64 start_offset,
	u64 end_offset) {
	struct ext4_range_node *node_start, *node_end,
		*best_start, *best_end;

	/* these value should *NOT* be "K" alloced
	 * but rather taken from a static buffer of a constant size
	 * lets say 5000
	 *
	 *
	 * need to answer question, how much DIO's can be run at same time on a disk?
	 */


	node_start = (struct ext4_range_node*) kmalloc(sizeof(struct ext4_range_node*), GFP_KERNEL);
	node_end = (struct ext4_range_node*) kmalloc(sizeof(struct ext4_range_node*), GFP_KERNEL);

	*node_start = (struct ext4_range_node) {
		.refs = 0,
		.inode = inode,
		.offset = start_offset,
		.value = BOUNDRY_START,
	};
	*node_end = (struct ext4_range_node) {
		.refs = 0,
		.inode = inode,
		.offset = end_offset,
		.value = BOUNDRY_END,
	};

//	struct lockdep_class_key __key;
//	mutex_init_with_key(node_start->internal_mutex, __key);
//	mutex_init_with_key(node_end->internal_mutex, __key);

	while (true) {
		mutex_lock(&lock);

		best_start = find_first_le(inode, node_start->offset);
		best_end = find_first_le(inode, node_end->offset);

		mutex_unlock(&lock);

		if (best_start != NULL && best_start->inode == node_start->inode
			&& (best_start->offset == node_start->offset || best_start->value == BOUNDRY_START)) {
			/* we wait for best_start's lock */
			continue;
		}

		if (best_end != NULL && best_end->inode == node_end->inode
			&& (best_end->value == BOUNDRY_START || best_end->offset >= node_start->offset )) {
			/* we wait for best_end's lock */
			continue;
		}
		/* there is no overlap after this point */
		break;
	}


	return ;
}


