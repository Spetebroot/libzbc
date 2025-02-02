/*
 * This file is part of libzbc.
 *
 * Copyright (C) 2009-2014, HGST, Inc. All rights reserved.
 * Copyright (C) 2016, Western Digital. All rights reserved.
 *
 * This software is distributed under the terms of the BSD 2-clause license,
 * "as is," without technical support, and WITHOUT ANY WARRANTY, without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. You should have received a copy of the BSD 2-clause license along
 * with libzbc. If not, see  <http://opensource.org/licenses/BSD-2-Clause>.
 *
 * Authors: Damien Le Moal (damien.lemoal@wdc.com)
 *          Christoph Hellwig (hch@infradead.org)
 */

#include "zbc.h"

#include <string.h>
#include <limits.h>
#include <assert.h>

/*
 * Log level.
 */
int zbc_log_level = ZBC_LOG_WARNING;

/*
 * Backend drivers.
 */
static struct zbc_drv *zbc_drv[] = {
	&zbc_block_drv,
	&zbc_scsi_drv,
	&zbc_ata_drv,
	&zbc_fake_drv,
	NULL
};

/**
 * Sense key strings.
 */
static struct zbc_sg_sk_s {
	enum zbc_sk	sk;
	const char	*sk_name;
} zbc_sg_sk_list[] = {
	{ ZBC_SK_ILLEGAL_REQUEST,	"Illegal-request"	},
	{ ZBC_SK_DATA_PROTECT,		"Data-protect"		},
	{ ZBC_SK_ABORTED_COMMAND,	"Aborted-command"	},
	{ 0,				NULL }
};

/**
 * Sense code qualifiers strings.
 */
static struct zbc_sg_asc_ascq_s {
	enum zbc_asc_ascq	asc_ascq;
	const char		*ascq_name;
} zbc_sg_asc_ascq_list[] = {

	{
		ZBC_ASC_INVALID_FIELD_IN_CDB,
		"Invalid-field-in-cdb"
	},
	{
		ZBC_ASC_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE,
		"Logical-block-address-out-of-range"
	},
	{
		ZBC_ASC_UNALIGNED_WRITE_COMMAND,
		"Unaligned-write-command"
	},
	{
		ZBC_ASC_WRITE_BOUNDARY_VIOLATION,
		"Write-boundary-violation"
	},
	{
		ZBC_ASC_ATTEMPT_TO_READ_INVALID_DATA,
		"Attempt-to-read-invalid-data"
	},
	{
		ZBC_ASC_READ_BOUNDARY_VIOLATION,
		"Read-boundary-violation"
	},
	{
		ZBC_ASC_ZONE_IS_READ_ONLY,
		"Zone-is-read-only"
	},
	{
		ZBC_ASC_INSUFFICIENT_ZONE_RESOURCES,
		"Insufficient-zone-resources"
	},
	{
		0,
		NULL
	}
};

/**
 * Per-thread local zbc_errno handling.
 */
__thread struct zbc_errno zerrno;

/**
 * zbc_set_log_level - Set the library log level
 */
void zbc_set_log_level(char const *log_level)
{
	if (!log_level) {
		/* Set default */
		zbc_log_level = ZBC_LOG_ERROR;
		return;
	}

	if (strcmp(log_level, "none") == 0)
		zbc_log_level = ZBC_LOG_NONE;
	else if (strcmp(log_level, "warning") == 0)
		zbc_log_level = ZBC_LOG_WARNING;
	else if (strcmp(log_level, "error") == 0)
		zbc_log_level = ZBC_LOG_ERROR;
	else if (strcmp(log_level, "info") == 0)
		zbc_log_level = ZBC_LOG_INFO;
	else if (strcmp(log_level, "debug") == 0)
		zbc_log_level = ZBC_LOG_DEBUG;
	else
		fprintf(stderr, "Unknown log level \"%s\"\n",
			log_level);
}

/**
 * zbc_device_type_str - Returns a devicetype name
 */
const char *zbc_device_type_str(enum zbc_dev_type type)
{
	switch (type) {
	case ZBC_DT_BLOCK:
		return "Zoned block device";
	case ZBC_DT_SCSI:
		return "SCSI ZBC device";
	case ZBC_DT_ATA:
		return "ATA ZAC device";
	case ZBC_DT_FAKE:
		return "Emulated zoned block device";
	case ZBC_DT_UNKNOWN:
	default:
		return "Unknown-device-type";
	}
}

/**
 * zbc_device_model_str - Returns a device zone model name
 */
const char *zbc_device_model_str(enum zbc_dev_model model)
{
	switch (model) {
	case ZBC_DM_HOST_AWARE:
		return "Host-aware";
	case ZBC_DM_HOST_MANAGED:
		return "Host-managed";
	case ZBC_DM_DEVICE_MANAGED:
		return "Device-managed";
	case ZBC_DM_STANDARD:
		return "Standard block device";
	case ZBC_DM_DRIVE_UNKNOWN:
	default:
		return "Unknown-device-model";
	}
}

/**
 * zbc_zone_type_str - returns a string describing a zone type
 */
const char *zbc_zone_type_str(enum zbc_zone_type type)
{
	switch (type) {
	case ZBC_ZT_CONVENTIONAL:
		return "Conventional";
	case ZBC_ZT_SEQUENTIAL_REQ:
		return "Sequential-write-required";
	case ZBC_ZT_SEQUENTIAL_PREF:
		return "Sequential-write-preferred";
	case ZBC_ZT_UNKNOWN:
	default:
		return "Unknown-zone-type";
	}
}

/**
 * zbc_zone_cond_str - Returns a string describing a zone condition
 */
const char *zbc_zone_condition_str(enum zbc_zone_condition cond)
{
	switch (cond) {
	case ZBC_ZC_NOT_WP:
		return "Not-write-pointer";
	case ZBC_ZC_EMPTY:
		return "Empty";
	case ZBC_ZC_IMP_OPEN:
		return "Implicit-open";
	case ZBC_ZC_EXP_OPEN:
		return "Explicit-open";
	case ZBC_ZC_CLOSED:
		return "Closed";
	case ZBC_ZC_RDONLY:
		return "Read-only";
	case ZBC_ZC_FULL:
		return "Full";
	case ZBC_ZC_OFFLINE:
		return "Offline";
	default:
		return "Unknown-zone-condition";
	}
}

/**
 * zbc_errno - Get detailed error code of last operation
 */
void zbc_errno(struct zbc_device *dev, struct zbc_errno *err)
{
	memcpy(err, &zerrno, sizeof(struct zbc_errno));
}

/**
 * zbc_sk_str - Returns a string describing a sense key
 */
const char *zbc_sk_str(enum zbc_sk sk)
{
	static char sk_buf[64];
	int i = 0;

	while (zbc_sg_sk_list[i].sk != 0) {
		if (sk == zbc_sg_sk_list[i].sk)
			return zbc_sg_sk_list[i].sk_name;
		i++;
	}

	sprintf(sk_buf, "Unknown-sense-key 0x%02X", (int)sk);

	return sk_buf;
}

/**
 * zbc_asc_ascq_str - Returns a string describing a sense code
 */
const char *zbc_asc_ascq_str(enum zbc_asc_ascq asc_ascq)
{
	static char asc_buf[64];
	int i = 0;

	while (zbc_sg_asc_ascq_list[i].asc_ascq != 0) {
		if (asc_ascq == zbc_sg_asc_ascq_list[i].asc_ascq)
			return zbc_sg_asc_ascq_list[i].ascq_name;
		i++;
	}

	sprintf(asc_buf,
		"Unknown-additional-sense-code-qualifier 0x%02X",
		(int)asc_ascq);

	return asc_buf;
}

static int zbc_realpath(const char *filename, char **path)
{
	char *p;
	int ret = 0;

	/* Follow symlinks (required for device mapped devices) */
	p = realpath(filename, NULL);
	if (p) {
		*path = p;
	} else {
		ret = -errno;
		zbc_error("%s: Failed to get real path %d (%s)\n",
			  filename,
			  errno, strerror(errno));
	}
	
	return ret;
}

/**
 * zbc_device_is_zoned - Test if a physical device is zoned.
 */
int zbc_device_is_zoned(const char *filename,
			bool fake,
			struct zbc_device_info *info)
{
	struct zbc_device *dev = NULL;
	char *path = NULL;
	int ret, i;

	ret = zbc_realpath(filename, &path);
	if (ret)
		return ret;

	/* Test all backends until one accepts the drive. */
	for (i = 0; zbc_drv[i]; i++) {
		ret = zbc_drv[i]->zbd_open(filename, O_RDONLY, &dev);
		if (ret == 0) {
			/* This backend accepted the device */
			dev->zbd_drv = zbc_drv[i];
			break;
		}
		if (ret != -ENXIO)
			goto out;
	}

	if (dev && dev->zbd_drv) {
		if (dev->zbd_drv == &zbc_fake_drv && !fake) {
			ret = 0;
		} else {
			ret = 1;
			if (info)
				memcpy(info, &dev->zbd_info,
				       sizeof(struct zbc_device_info));
		}
		dev->zbd_drv->zbd_close(dev);
	} else {
		if ((ret != -EPERM) && (ret != -EACCES))
			ret = 0;
	}

out:
	free(path);

	return ret;
}

/**
 * zbc_open - open a ZBC device
 */
int zbc_open(const char *filename, int flags, struct zbc_device **pdev)
{
	struct zbc_device *dev = NULL;
	unsigned int allowed_drv;
	char *path = NULL;
	int ret, i;

	ret = zbc_realpath(filename, &path);
	if (ret)
		return ret;

	allowed_drv = flags & ZBC_O_DRV_MASK;
	if (!allowed_drv)
		allowed_drv = ZBC_O_DRV_MASK;
#ifndef HAVE_LINUX_BLKZONED_H
	allowed_drv &= ~ZBC_O_DRV_BLOCK;
#endif

	/* Test all backends until one accepts the drive */
	ret = -ENODEV;
	for (i = 0; zbc_drv[i] != NULL; i++) {

		if (!(zbc_drv[i]->flag & allowed_drv))
			continue;

		ret = zbc_drv[i]->zbd_open(path, flags, &dev);
		switch (ret) {
		case 0:
			/* This backend accepted the drive */
			dev->zbd_drv = zbc_drv[i];
			*pdev = dev;
			goto out;
		case -ENXIO:
			continue;
		default:
			goto out;
		}

	}

out:
	free(path);
	return ret;
}

/**
 * zbc_close - close a ZBC Device
 */
int zbc_close(struct zbc_device *dev)
{
	return dev->zbd_drv->zbd_close(dev);
}

/**
 * zbc_get_device_info - Get a ZBC device information
 */
void zbc_get_device_info(struct zbc_device *dev, struct zbc_device_info *info)
{
	memcpy(info, &dev->zbd_info, sizeof(struct zbc_device_info));
}

/**
 * zbc_print_device_info - Print a device information
 */
void zbc_print_device_info(struct zbc_device_info *info, FILE *out)
{
	char tmp[64];

	fprintf(out,
		"    Vendor ID: %s\n",
		info->zbd_vendor_id);
	if (info->zbd_model == ZBC_DM_STANDARD) {
		fprintf(out,
			"    %s interface, standard block device\n",
			zbc_device_type_str(info->zbd_type));
	} else {
		fprintf(out,
			"    %s interface, %s zone model\n",
			zbc_device_type_str(info->zbd_type),
			zbc_device_model_str(info->zbd_model));
	}
	fprintf(out,
		"    %llu 512-bytes sectors\n",
		(unsigned long long) info->zbd_sectors);
	fprintf(out,
		"    %llu logical blocks of %u B\n",
		(unsigned long long) info->zbd_lblocks,
		(unsigned int) info->zbd_lblock_size);
	fprintf(out,
		"    %llu physical blocks of %u B\n",
		(unsigned long long) info->zbd_pblocks,
		(unsigned int) info->zbd_pblock_size);
	fprintf(out,
		"    %.03F GB capacity\n",
		(double)(info->zbd_sectors << 9) / 1000000000);

	if (info->zbd_model == ZBC_DM_HOST_MANAGED ||
	    info->zbd_model == ZBC_DM_HOST_AWARE)
		fprintf(out,
			"    Read commands are %s\n",
			(info->zbd_flags & ZBC_UNRESTRICTED_READ) ?
			"unrestricted" : "restricted");

	fprintf(out,
		"    %llu KiB max R/W size\n",
		(unsigned long long)(info->zbd_max_rw_sectors << 9) / 1024);

	if (info->zbd_model == ZBC_DM_HOST_MANAGED) {

		if (info->zbd_max_nr_open_seq_req == ZBC_NO_LIMIT)
			strcpy(tmp, "unlimited");
		else
			sprintf(tmp, "%u",
				(unsigned int) info->zbd_max_nr_open_seq_req);
		fprintf(out,
			"    Maximum number of open sequential write "
			"required zones: %s\n", tmp);

	} else if (info->zbd_model == ZBC_DM_HOST_AWARE) {

		if (info->zbd_opt_nr_open_seq_pref == ZBC_NOT_REPORTED)
			strcpy(tmp, "not reported");
		else
			sprintf(tmp, "%u",
				(unsigned int)info->zbd_opt_nr_open_seq_pref);
		fprintf(out,
			"    Optimal number of open sequential write "
			"preferred zones: %s\n", tmp);

		if (info->zbd_opt_nr_non_seq_write_seq_pref == ZBC_NOT_REPORTED)
			strcpy(tmp, "not reported");
		else
			sprintf(tmp, "%u",
				(unsigned int)info->zbd_opt_nr_non_seq_write_seq_pref);

		fprintf(out,
			"    Optimal number of non-sequentially written "
			"sequential write preferred zones: %s\n", tmp);

	}

	fflush(out);
}

/**
 * zbc_report_zones - Get zone information
 */
int zbc_report_zones(struct zbc_device *dev, uint64_t sector,
		     enum zbc_reporting_options ro,
		     struct zbc_zone *zones, unsigned int *nr_zones)
{
        unsigned int n, nz = 0;
	uint64_t last_sector;
	int ret;

	if (!zbc_test_mode(dev) &&
	    sector >= dev->zbd_info.zbd_sectors) {
		/* No zones to report beyond drive capacity */
		*nr_zones = 0;
		return 0;
	}

	if (!zones) {
		/* Get the number of zones */
		*nr_zones = 0;
		return (dev->zbd_drv->zbd_report_zones)(dev, sector,
							zbc_ro_mask(ro),
							NULL, nr_zones);
	}

        /* Get zones information */
        while (nz < *nr_zones) {

		n = *nr_zones - nz;
		ret = (dev->zbd_drv->zbd_report_zones)(dev, sector,
					zbc_ro_mask(ro) | ZBC_RO_PARTIAL,
					&zones[nz], &n);
		if (ret != 0) {
			zbc_error("%s: Get zones from sector %llu failed %d (%s)\n",
				  dev->zbd_filename,
				  (unsigned long long) sector,
				  ret, strerror(-ret));
			return ret;
		}

		if (n == 0)
			break;

		nz += n;
		last_sector = zones[nz - 1].zbz_start +
			zones[nz - 1].zbz_length;

		if (last_sector >= dev->zbd_info.zbd_sectors)
			break;

		sector = last_sector;

        }

	*nr_zones = nz;

	return 0;
}

/**
 * zbc_list_zones - Get zone information
 */
int zbc_list_zones(struct zbc_device *dev, uint64_t sector,
		   enum zbc_reporting_options ro,
		   struct zbc_zone **pzones, unsigned int *pnr_zones)
{
	struct zbc_zone *zones = NULL;
	unsigned int nr_zones = 0;
	int ret;

	/* Get total number of zones */
	ret = zbc_report_nr_zones(dev, sector, zbc_ro_mask(ro), &nr_zones);
	if (ret < 0)
		return ret;

	if (!nr_zones)
		goto out;

	zbc_debug("%s: %d zones\n",
		  dev->zbd_filename,
		  nr_zones);

	/* Allocate zone array */
	zones = (struct zbc_zone *) calloc(nr_zones, sizeof(struct zbc_zone));
	if (!zones)
		return -ENOMEM;

	/* Get zones information */
	ret = zbc_report_zones(dev, sector, zbc_ro_mask(ro), zones, &nr_zones);
	if (ret != 0) {
		zbc_error("%s: zbc_report_zones failed %d\n",
			  dev->zbd_filename, ret);
		free(zones);
		return ret;
	}

out:
	*pzones = zones;
	*pnr_zones = nr_zones;

	return 0;
}

/**
 * zbc_zone_operation - Execute an operation on a zone
 */
int zbc_zone_operation(struct zbc_device *dev, uint64_t sector,
		       enum zbc_zone_op op, unsigned int flags)
{

	if (!zbc_test_mode(dev) &&
	    (!(flags & ZBC_OP_ALL_ZONES)) &&
	    !zbc_dev_sect_laligned(dev, sector))
		return -EINVAL;

	/* Execute the operation */
	return (dev->zbd_drv->zbd_zone_op)(dev, sector, op, flags);
}

/**
 * Convert vector buffer sizes to bytes for the vector
 * range between @offset and @offset +@size.
 */
static int zbc_iov_convert(struct iovec *_iov, const struct iovec *iov,
			   int iovcnt, size_t sector_offset, size_t sectors)
{
	size_t size = sectors << 9;
	size_t offset = sector_offset << 9;
	size_t length, count = 0;
        int i, j = 0;

	for (i = 0; i < iovcnt && count < size; i++) {

		length = iov[i].iov_len << 9;
		if (offset >= length) {
			offset -= length;
			continue;
		}

		_iov[j].iov_base = iov[i].iov_base + offset;
		length -= offset;
		offset = 0;

		if (count + length > size)
			length = size - count;
		_iov[j].iov_len = length;
		count += length;
		j++;

	}

	return j;
}

/**
 * zbc_do_preadv - Execute a vector read
 */
static ssize_t zbc_do_preadv(struct zbc_device *dev,
			     const struct iovec *iov, int iovcnt,
			     uint64_t offset)
{
	size_t max_count = dev->zbd_info.zbd_max_rw_sectors;
	size_t count = zbc_iov_count(iov, iovcnt);
	struct iovec rd_iov[iovcnt];
	size_t rd_iov_count = 0, rd_iov_offset = 0;
	int rd_iovcnt;
	ssize_t ret;

	if (count << 9 > SSIZE_MAX)
		return -EINVAL;

	if (zbc_test_mode(dev)) {
		if (!count) {
			zbc_error("%s: zero-length read at sector %llu\n",
				  dev->zbd_filename,
				  (unsigned long long) offset);
			return -EINVAL;
		}
	} else {
		if (!zbc_dev_sect_laligned(dev, count) ||
		    !zbc_dev_sect_laligned(dev, offset)) {
			zbc_error("%s: Unaligned read %zu sectors at "
				  "sector %llu\n",
				  dev->zbd_filename,
				  count, (unsigned long long) offset);
			return -EINVAL;
		}

		if ((offset + count) > dev->zbd_info.zbd_sectors)
			count = dev->zbd_info.zbd_sectors - offset;
		if (!count || offset >= dev->zbd_info.zbd_sectors)
			return 0;
	}

	zbc_debug("%s: Read %zu sectors at sector %llu, %d vectors\n",
		  dev->zbd_filename,
		  count, (unsigned long long) offset, iovcnt);

	if (zbc_test_mode(dev) && count == 0) {
		zbc_iov_convert(rd_iov, iov, iovcnt, 0, count);
		ret = (dev->zbd_drv->zbd_preadv)(dev, rd_iov, iovcnt, offset);
		if (ret < 0) {
			zbc_error("%s: read of zero sectors at sector %llu "
				  "failed %ld (%s)\n",
				  dev->zbd_filename,
				  (unsigned long long) offset,
				  -ret, strerror(-ret));
		}
		return ret;
	}

	while (rd_iov_offset < count) {

		rd_iov_count = count - rd_iov_offset;
		if (rd_iov_count > max_count)
			rd_iov_count = max_count;

		rd_iovcnt = zbc_iov_convert(rd_iov, iov, iovcnt,
					    rd_iov_offset, rd_iov_count);

		ret = (dev->zbd_drv->zbd_preadv)(dev, rd_iov, rd_iovcnt,
						 offset);
		if (ret <= 0) {
			zbc_error("%s: Read %zu sectors at sector %llu "
				  "failed %zd (%s)\n",
				  dev->zbd_filename,
				  rd_iov_count, (unsigned long long) offset,
				  -ret, strerror(-ret));
			return ret ? ret : -EIO;
		}

		offset += ret;
		rd_iov_offset += ret;

	}

	return count;
}

/**
 * zbc_pread - Read sectors from a device
 */
ssize_t zbc_pread(struct zbc_device *dev, void *buf, size_t count,
		  uint64_t offset)
{
	const struct iovec iov = { buf, count };

	return zbc_do_preadv(dev, &iov, 1, offset);
}

/**
 * zbc_preadv - Vector read sectors from a device
 */
ssize_t zbc_preadv(struct zbc_device *dev,
		   const struct iovec *iov, int iovcnt, uint64_t offset)
{
	if (!iov || iovcnt <= 0)
		return -EINVAL;

	return zbc_do_preadv(dev, iov, iovcnt, offset);
}

/**
 * zbc_do_pwritev - Execute a vector write
 */
static ssize_t zbc_do_pwritev(struct zbc_device *dev,
			      const struct iovec *iov, int iovcnt,
			      uint64_t offset)
{
	size_t max_count = dev->zbd_info.zbd_max_rw_sectors;
	size_t count = zbc_iov_count(iov, iovcnt);
	struct iovec wr_iov[iovcnt];
	size_t wr_iov_count = 0, wr_iov_offset = 0;
	int wr_iovcnt;
	ssize_t ret;

	if (count << 9 > SSIZE_MAX)
		return -EINVAL;

	if (zbc_test_mode(dev)) {
		if (!count) {
			zbc_error("%s: zero-length write at sector %llu\n",
				  dev->zbd_filename,
				  (unsigned long long) offset);
			return -EINVAL;
		}
	} else {
		if (!zbc_dev_sect_paligned(dev, count) ||
		    !zbc_dev_sect_paligned(dev, offset)) {
			zbc_error("%s: Unaligned write %zu sectors at "
				  "sector %llu\n",
				  dev->zbd_filename,
				  count, (unsigned long long) offset);
			return -EINVAL;
		}

		if ((offset + count) > dev->zbd_info.zbd_sectors)
			count = dev->zbd_info.zbd_sectors - offset;
		if (!count || offset >= dev->zbd_info.zbd_sectors)
			return 0;
	}

	zbc_debug("%s: Write %zu sectors at sector %llu, %d vectors\n",
		  dev->zbd_filename,
		  count, (unsigned long long) offset, iovcnt);

	if (zbc_test_mode(dev) && count == 0) {
		zbc_iov_convert(wr_iov, iov, iovcnt, 0, count);
		ret = (dev->zbd_drv->zbd_pwritev)(dev, wr_iov, iovcnt, offset);
		if (ret < 0) {
			zbc_error("%s: Write of zero sectors at sector %llu "
				  "failed %ld (%s)\n",
				  dev->zbd_filename,
				  (unsigned long long) offset,
				  -ret, strerror(-ret));
		}
		return ret;
	}

	while (wr_iov_offset < count) {

		wr_iov_count = count - wr_iov_offset;
		if (wr_iov_count > max_count)
			wr_iov_count = max_count;

		wr_iovcnt = zbc_iov_convert(wr_iov, iov, iovcnt,
					    wr_iov_offset, wr_iov_count);

		ret = (dev->zbd_drv->zbd_pwritev)(dev, wr_iov, wr_iovcnt,
						  offset);
		if (ret <= 0) {
			zbc_error("%s: Write %zu sectors at sector %llu failed "
				  "%zd (%s)\n",
				  dev->zbd_filename,
				  wr_iov_count, (unsigned long long) offset,
				  -ret, strerror(-ret));
			return ret ? ret : -EIO;
		}

		offset += ret;
		wr_iov_offset += ret;

	}

	return count;
}

/**
 * zbc_pwrite - Write sectors form a device
 */
ssize_t zbc_pwrite(struct zbc_device *dev, const void *buf, size_t count,
		   uint64_t offset)
{
	const struct iovec iov = { (void *)buf, count };

	return zbc_do_pwritev(dev, &iov, 1, offset);
}

/**
 * zbc_pwritev - Vector write sectors form a device
 */
ssize_t zbc_pwritev(struct zbc_device *dev, const struct iovec *iov, int iovcnt,
		    uint64_t offset)
{
	if (!iov || iovcnt <= 0)
		return -EINVAL;

	return zbc_do_pwritev(dev, iov, iovcnt, offset);
}

/**
 * zbc_map_iov - Map a buffer to an IOV
 */
int zbc_map_iov(const void *buf, size_t sectors,
		struct iovec *iov, int iovcnt, size_t iovlen)
{
	size_t len;
	int i = 0;

	if (!buf || !sectors || !iov || iovcnt <= 0 ||
	    sectors > iovcnt * iovlen)
		return -EINVAL;

	while (sectors) {

		if (sectors > iovlen)
			len = iovlen;
		else
			len = sectors;

		iov[i].iov_base = (void*)buf;
		iov[i].iov_len = len;

		buf += len << 9;
		sectors -= len;
		i++;
	}

	return i;
}

/**
 * zbc_flush - flush a device write cache
 */
int zbc_flush(struct zbc_device *dev)
{
	return (dev->zbd_drv->zbd_flush)(dev);
}

/**
 * zbc_set_zones - Configure zones of an emulated device
 */
int zbc_set_zones(struct zbc_device *dev,
		  uint64_t conv_sz, uint64_t zone_sz)
{

	/* Do this only if supported */
	if (!dev->zbd_drv->zbd_set_zones)
		return -ENXIO;

	if (!zbc_dev_sect_paligned(dev, conv_sz) ||
	    !zbc_dev_sect_paligned(dev, zone_sz))
		return -EINVAL;

	return (dev->zbd_drv->zbd_set_zones)(dev, conv_sz, zone_sz);
}

/**
 * zbc_set_write_pointer - Change an emulated device zone write pointers
 */
int zbc_set_write_pointer(struct zbc_device *dev,
			  uint64_t sector, uint64_t wp_sector)
{

	/* Do this only if supported */
	if (!dev->zbd_drv->zbd_set_wp)
		return -ENXIO;

	if (!zbc_dev_sect_paligned(dev, sector) ||
	    !zbc_dev_sect_paligned(dev, wp_sector))
		return -EINVAL;

	return (dev->zbd_drv->zbd_set_wp)(dev, sector, wp_sector);
}

