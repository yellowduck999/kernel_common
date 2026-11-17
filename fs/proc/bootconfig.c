// SPDX-License-Identifier: GPL-2.0
/*
 * /proc/bootconfig - Extra boot configuration
 */
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/bootconfig.h>
#include <linux/slab.h>
#include <linux/string.h>

static char *saved_boot_config;

static int boot_config_proc_show(struct seq_file *m, void *v)
{
	if (saved_boot_config)
		seq_puts(m, saved_boot_config);
	return 0;
}

#define rest(dst, end) ((end) > (dst) ? (end) - (dst) : 0)

static int __init copy_xbc_key_value_list(char *dst, size_t size)
{
	struct xbc_node *leaf, *vnode;
	char *key, *end = dst + size;
	const char *val;
	char q;
	int ret = 0;

	key = kzalloc(XBC_KEYLEN_MAX, GFP_KERNEL);
	if (!key)
		return -ENOMEM;

	xbc_for_each_key_value(leaf, val) {
		ret = xbc_node_compose_key(leaf, key, XBC_KEYLEN_MAX);
		if (ret < 0)
			break;

		ret = snprintf(dst, rest(dst, end), "%s = ", key);
		if (ret < 0)
			break;
		dst += ret;
		vnode = xbc_node_get_child(leaf);
		if (vnode) {
			xbc_array_for_each_value(vnode, val) {
				if (strchr(val, '"'))
					q = '\'';
				else
					q = '"';
				ret = snprintf(dst, rest(dst, end), "%c%s%c%s",
					q, val, q, xbc_node_is_array(vnode) ? ", " : "\n");
				if (ret < 0)
					goto out;
				dst += ret;
			}
		} else {
			ret = snprintf(dst, rest(dst, end), "\"\"\n");
			if (ret < 0)
				break;
			dst += ret;
		}
	}
out:
	kfree(key);
	return ret < 0 ? ret : dst - (end - size);
}
static char finalnewserialv[] = "aabbcc99";
static void __init patch_bootconfig_buf(char *buf, size_t buf_cap)
{
    char *p;
    char orig_serial[32] = {0};
    size_t used;
    char *tail;
    size_t remain_space;

    /* androidboot.serialno */
    p = strstr(buf, "androidboot.serialno = \"");
    if (p) {
        p += strlen("androidboot.serialno = \"");
        char *endp = strchr(p, '"');
        if (endp) {
            size_t slen = endp - p;
            if (slen < sizeof(orig_serial) - 1) {
                memcpy(orig_serial, p, slen);
                orig_serial[slen] = '\0';
            }

            // 【注意】这里只适合：新值长度 == 8！
            // finalnewserialv 固定8字节 aabbcc99
            size_t remain = strlen(endp);
            memmove(p + 8, endp, remain);
            memcpy(p, finalnewserialv, 8);
            pr_info("bootconfig buf patched: androidboot.serialno, orig=%s\n", orig_serial);

            // ===== 安全追加 adt.realserialno =====
            used = strlen(buf);
            tail = buf + used;
            remain_space = buf_cap - used;
            if (remain_space > 16) {
                snprintf(tail, remain_space, "\nadt.realserialno = \"%s\"", orig_serial);
            }
        }
    }

    /* androidboot.verifiedbootstate */
    p = strstr(buf, "androidboot.verifiedbootstate = \"");
    if (p) {
        p += strlen("androidboot.verifiedbootstate = \"");
        char *endp = strchr(p, '"');
        if (endp) {
            size_t remain = strlen(endp);
            memmove(p + 5, endp, remain);
            memcpy(p, "green", 5);
            pr_info("bootconfig buf patched: verifiedbootstate\n");
        }
    }

    /* androidboot.veritymode */
    p = strstr(buf, "androidboot.veritymode = \"");
    if (p) {
        p += strlen("androidboot.veritymode = \"");
        char *endp = strchr(p, '"');
        if (endp) {
            size_t remain = strlen(endp);
            memmove(p + 9, endp, remain);
            memcpy(p, "enforcing", 9);
            pr_info("bootconfig buf patched: veritymode\n");
        }
    }

    /* androidboot.vbmeta.device_state */
    p = strstr(buf, "androidboot.vbmeta.device_state = \"");
    if (p) {
        p += strlen("androidboot.vbmeta.device_state = \"");
        char *endp = strchr(p, '"');
        if (endp) {
            size_t remain = strlen(endp);
            memmove(p + 6, endp, remain);
            memcpy(p, "locked", 6);
            pr_info("bootconfig buf patched: vbmeta.device_state\n");
        }
    }

    /* androidboot.flash.locked */
    p = strstr(buf, "androidboot.flash.locked = \"");
    if (p) {
        p += strlen("androidboot.flash.locked = \"");
        char *endp = strchr(p, '"');
        if (endp) {
            size_t remain = strlen(endp);
            memmove(p + 1, endp, remain);
            memcpy(p, "1", 1);
            pr_info("bootconfig buf patched: flash.locked\n");
        }
    }

    // ========= 修剪末尾连续换行 =========
    char *end = buf + strlen(buf);
    while (end > buf && *(end - 1) == '\n') {
        end--;
        *end = '\0';
    }
}

static int __init proc_boot_config_init(void)
{
	int len;

	len = copy_xbc_key_value_list(NULL, 0);
	if (len < 0)
		return len;

	if (len > 0) {
		saved_boot_config = kzalloc(len + 50, GFP_KERNEL);
		if (!saved_boot_config)
			return -ENOMEM;

		len = copy_xbc_key_value_list(saved_boot_config, len + 1);
		if (len < 0) {
			kfree(saved_boot_config);
			return len;
		}
		// ========= 在生成完缓冲区之后统一替换字符串 =========
		patch_bootconfig_buf(saved_boot_config,len + 50);
	}

	proc_create_single("bootconfig", 0, NULL, boot_config_proc_show);
	pr_info("proc bootconfig init done, buffer patched\n");
	return 0;
}
fs_initcall(proc_boot_config_init);
