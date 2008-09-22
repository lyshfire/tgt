/*
 * backing store routine
 *
 * Copyright (C) 2007 FUJITA Tomonori <tomof@acm.org>
 * Copyright (C) 2007 Mike Christie <michaelc@cs.wisc.edu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/epoll.h>

#include "list.h"
#include "tgtd.h"
#include "util.h"
#include "bs_thread.h"

static LIST_HEAD(bst_list);

int register_backingstore_template(struct backingstore_template *bst)
{
	list_add(&bst->backingstore_siblings, &bst_list);

	return 0;
}

struct backingstore_template *get_backingstore_template(const char *name)
{
	struct backingstore_template *bst;

	list_for_each_entry(bst, &bst_list, backingstore_siblings) {
		if (!strcmp(name, bst->bs_name))
			return bst;
	}
	return NULL;
}

/* threading helper functions */

static void *bs_thread_ack_fn(void *arg)
{
	struct bs_thread_info *info = arg;
	int command, ret, nr;
	struct scsi_cmd *cmd;

retry:
	ret = read(info->command_fd[0], &command, sizeof(command));
	if (ret < 0) {
		eprintf("ack pthread will be dead, %m\n");
		if (errno == EAGAIN || errno == EINTR)
			goto retry;

		goto out;
	}

	pthread_mutex_lock(&info->finished_lock);
retest:
	if (list_empty(&info->finished_list)) {
		pthread_cond_wait(&info->finished_cond, &info->finished_lock);
		goto retest;
	}

	while (!list_empty(&info->finished_list)) {
		cmd = list_first_entry(&info->finished_list,
				 struct scsi_cmd, bs_list);

		dprintf("found %p\n", cmd);

		list_del(&cmd->bs_list);
		list_add(&cmd->bs_list, &info->ack_list);
	}

	pthread_mutex_unlock(&info->finished_lock);

	nr = 1;
rewrite:
	ret = write(info->done_fd[1], &nr, sizeof(nr));
	if (ret < 0) {
		eprintf("can't ack tgtd, %m\n");
		if (errno == EAGAIN || errno == EINTR)
			goto rewrite;

		goto out;
	}

	goto retry;
out:
	return NULL;
}

static void bs_thread_request_done(int fd, int events, void *data)
{
	struct bs_thread_info *info = data;
	struct scsi_cmd *cmd;
	int nr_events, ret;

	ret = read(info->done_fd[0], &nr_events, sizeof(nr_events));
	if (ret < 0) {
		eprintf("wrong wakeup\n");
		return;
	}

	while (!list_empty(&info->ack_list)) {
		cmd = list_first_entry(&info->ack_list,
				       struct scsi_cmd, bs_list);

		dprintf("back to tgtd, %p\n", cmd);

		list_del(&cmd->bs_list);
		cmd->scsi_cmd_done(cmd, scsi_get_result(cmd));
	}

rewrite:
	ret = write(info->command_fd[1], &nr_events, sizeof(nr_events));
	if (ret < 0) {
		eprintf("can't write done, %m\n");
		if (errno == EAGAIN || errno == EINTR)
			goto rewrite;

		return;
	}
}

static void *bs_thread_worker_fn(void *arg)
{
	struct bs_thread_info *info = arg;
	struct scsi_cmd *cmd;

	while (1) {
		pthread_mutex_lock(&info->pending_lock);
	retest:
		if (list_empty(&info->pending_list)) {
			pthread_cond_wait(&info->pending_cond, &info->pending_lock);
			if (info->stop) {
				pthread_mutex_unlock(&info->pending_lock);
				break;
			}
			goto retest;
		}

		cmd = list_first_entry(&info->pending_list,
				       struct scsi_cmd, bs_list);

		dprintf("got %p\n", cmd);

		list_del(&cmd->bs_list);
		pthread_mutex_unlock(&info->pending_lock);

		info->request_fn(cmd);

		pthread_mutex_lock(&info->finished_lock);
		list_add(&cmd->bs_list, &info->finished_list);
		pthread_mutex_unlock(&info->finished_lock);

		pthread_cond_signal(&info->finished_cond);
	}

	return NULL;
}

int bs_thread_open(struct bs_thread_info *info, request_func_t *rfn)
{
	int i, ret;

	info->request_fn = rfn;

	INIT_LIST_HEAD(&info->ack_list);
	INIT_LIST_HEAD(&info->finished_list);
	INIT_LIST_HEAD(&info->pending_list);

	pthread_cond_init(&info->finished_cond, NULL);
	pthread_cond_init(&info->pending_cond, NULL);

	pthread_mutex_init(&info->finished_lock, NULL);
	pthread_mutex_init(&info->pending_lock, NULL);

	ret = pipe(info->command_fd);
	if (ret)
		goto destroy_cond_mutex;

	ret = pipe(info->done_fd);
	if (ret)
		goto close_command_fd;

	ret = tgt_event_add(info->done_fd[0], EPOLLIN, bs_thread_request_done, info);
	if (ret)
		goto close_done_fd;

	ret = pthread_create(&info->ack_thread, NULL, bs_thread_ack_fn, info);
	if (ret)
		goto event_del;

	for (i = 0; i < ARRAY_SIZE(info->worker_thread); i++) {
		ret = pthread_create(&info->worker_thread[i], NULL,
				     bs_thread_worker_fn, info);
	}

rewrite:
	ret = write(info->command_fd[1], &ret, sizeof(ret));
	if (ret < 0) {
		eprintf("can't write done, %m\n");
		if (errno == EAGAIN || errno == EINTR)
			goto rewrite;
	}

	return 0;
event_del:
	tgt_event_del(info->done_fd[0]);
close_done_fd:
	close(info->done_fd[0]);
	close(info->done_fd[1]);
close_command_fd:
	close(info->command_fd[0]);
	close(info->command_fd[1]);
destroy_cond_mutex:
	pthread_cond_destroy(&info->finished_cond);
	pthread_cond_destroy(&info->pending_cond);
	pthread_mutex_destroy(&info->finished_lock);
	pthread_mutex_destroy(&info->pending_lock);

	return -1;
}

void bs_thread_close(struct bs_thread_info *info)
{
	int i;

	pthread_cancel(info->ack_thread);
	pthread_join(info->ack_thread, NULL);

	info->stop = 1;
	pthread_cond_broadcast(&info->pending_cond);

	for (i = 0; i < ARRAY_SIZE(info->worker_thread); i++)
		pthread_join(info->worker_thread[i], NULL);

	pthread_cond_destroy(&info->finished_cond);
	pthread_cond_destroy(&info->pending_cond);
	pthread_mutex_destroy(&info->finished_lock);
	pthread_mutex_destroy(&info->pending_lock);

	tgt_event_del(info->done_fd[0]);

	close(info->done_fd[0]);
	close(info->done_fd[1]);

	close(info->command_fd[0]);
	close(info->command_fd[1]);

	info->stop = 0;
}

int bs_thread_cmd_submit(struct scsi_cmd *cmd)
{
	struct scsi_lu *lu = cmd->dev;
	struct bs_thread_info *info = BS_THREAD_I(lu);

	pthread_mutex_lock(&info->pending_lock);

	list_add(&cmd->bs_list, &info->pending_list);

	pthread_mutex_unlock(&info->pending_lock);

	pthread_cond_signal(&info->pending_cond);

	set_cmd_async(cmd);

	return 0;
}
