#ifndef __TARGET_DAEMON_H
#define __TARGET_DAEMON_H

#include "log.h"
#include "dl.h"

/* makeshift */
#define	POLLS_PER_DRV	32
#define	RINGBUF_SIZE	(4096 * 8)

struct ringbuf_info {
	char *addr;
	uint32_t idx;
	uint32_t frame_size;
	uint32_t frame_nr;
};

struct tgtd_info {
	struct ringbuf_info ri;
};

extern int nl_init(int *, int *, struct ringbuf_info *);
extern int __nl_write(int fd, int type, char *data, int len);

extern int ipc_open(void);
extern void ipc_event_handle(struct driver_info *, int fd);

extern int tgt_device_init(void);
extern int tgt_device_create(int tid, uint64_t lun, char *path);
extern int tgt_device_destroy(int tid, uint64_t lun);
extern int tgt_target_create(int tid);
extern int tgt_target_destroy(int tid);
extern int tgt_target_bind(int tid, int host_no);

extern void pk_event_handle(struct tgtd_info *ti, int nl_fd);

extern uint64_t scsi_get_devid(uint8_t *pdu);
extern int scsi_cmd_process(int tid, uint8_t *pdu, int *len,
			    uint32_t datalen, unsigned long *uaddr, uint8_t *rw,
			    uint8_t *try_map, uint64_t *offset, uint64_t lun);
extern int scsi_cmd_done(int do_munmap, int do_free, uint64_t uaddr, int len);

#endif
