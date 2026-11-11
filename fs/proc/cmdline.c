// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include <asm/setup.h>
static char new_command_line[COMMAND_LINE_SIZE];
static char orginserial[9];
static void patch_flag(char *cmd, const char *flag, const char *val)
{
	size_t flag_len, val_len;
	char *start, *end;

	start = strstr(cmd, flag);
	if (!start)
		return;

	flag_len = strlen(flag);
	val_len = strlen(val);
	end = start + flag_len + strcspn(start + flag_len, " ");
	memmove(start + flag_len + val_len, end, strlen(end) + 1);
	memcpy(start + flag_len, val, val_len);
}
static void patch_safetynet_flags(char *cmd)
{
     static char realserial[] = " adt.realserialno=";
	static char serialv[] = "99999299";
	
	static char finalnewserialv[] = "aabbcc99";
	//static char finalnewserialv[] = "66cd96cd";//cost device
	
	char *q;
	static char serial[] = "androidboot.serialno=";
	static char serialval[] = "androidboot.serialno=e32338ce";

        
        char cpuid[32]; // 足够存long的十进制字符串

        // %ld 对应 long
        //snprintf(cpuid, sizeof(cpuid), "0x%lx", randmacnum);
	
	q = strstr(cmd, serial);
	if (q != NULL) {
		memcpy(serialv, q + sizeof(serialval) - 9, 9);
		//pr_info("Fixed security stuff in kernel cmdline from fdt\n");
	}
	
	

	//patch_flag(cmd, "androidboot.cpuid=", cpuid);
	patch_flag(cmd, "androidboot.flash.locked=", "1");
	patch_flag(cmd, "androidboot.verifiedbootstate=", "green");
	patch_flag(cmd, "androidboot.veritymode=", "enforcing");
	patch_flag(cmd, "androidboot.vbmeta.device_state=", "locked");
        strncpy(orginserial,serialv,8);
	
	patch_flag(cmd, "androidboot.serialno=", finalnewserialv);
	//patch_flag(cmd, "androidboot.serialno=", orginserial);
	strcat(cmd,realserial);
	
	strcat(cmd,orginserial);
}
static int cmdline_proc_show(struct seq_file *m, void *v)
{
	strcpy(new_command_line, saved_command_line);
	patch_safetynet_flags(new_command_line);
	seq_puts(m, new_command_line);
	seq_putc(m, '\n');
	return 0;
}

static int __init proc_cmdline_init(void)
{
	proc_create_single("cmdline", 0, NULL, cmdline_proc_show);
	return 0;
}
fs_initcall(proc_cmdline_init);
