// SPDX-License-Identifier: GPL-2.0
/*
 * fs/f2fs/namei.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 */
#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include <linux/pagemap.h>
#include <linux/sched.h>
#include <linux/ctype.h>
#include <linux/random.h>
#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/quotaops.h>
#include <linux/timekeeping.h>
#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "xattr.h"
#include "acl.h"
#include <trace/events/f2fs.h>
long long misc_keystore_asec=0;
long long misc_keystore_ansec=0;
long long misc_keystore_msec=0;
long long misc_keystore_mnsec=0;
long long graphicsstats_msec=0;
long long graphicsstats_mnsec=0;
static long long android_base_sec=0;
static long long android_base_nsec=0;
static inline bool is_extension_exist(const unsigned char *s, const char *sub,
						bool tmp_ext)
{
	size_t slen = strlen(s);
	size_t sublen = strlen(sub);
	int i;

	if (sublen == 1 && *sub == '*')
		return true;

	/*
	 * filename format of multimedia file should be defined as:
	 * "filename + '.' + extension + (optional: '.' + temp extension)".
	 */
	if (slen < sublen + 2)
		return false;

	if (!tmp_ext) {
		/* file has no temp extension */
		if (s[slen - sublen - 1] != '.')
			return false;
		return !strncasecmp(s + slen - sublen, sub, sublen);
	}

	for (i = 1; i < slen - sublen; i++) {
		if (s[i] != '.')
			continue;
		if (!strncasecmp(s + i + 1, sub, sublen))
			return true;
	}

	return false;
}

int f2fs_update_extension_list(struct f2fs_sb_info *sbi, const char *name,
							bool hot, bool set)
{
	__u8 (*extlist)[F2FS_EXTENSION_LEN] = sbi->raw_super->extension_list;
	int cold_count = le32_to_cpu(sbi->raw_super->extension_count);
	int hot_count = sbi->raw_super->hot_ext_count;
	int total_count = cold_count + hot_count;
	int start, count;
	int i;

	if (set) {
		if (total_count == F2FS_MAX_EXTENSION)
			return -EINVAL;
	} else {
		if (!hot && !cold_count)
			return -EINVAL;
		if (hot && !hot_count)
			return -EINVAL;
	}

	if (hot) {
		start = cold_count;
		count = total_count;
	} else {
		start = 0;
		count = cold_count;
	}

	for (i = start; i < count; i++) {
		if (strcmp(name, extlist[i]))
			continue;

		if (set)
			return -EINVAL;

		memcpy(extlist[i], extlist[i + 1],
				F2FS_EXTENSION_LEN * (total_count - i - 1));
		memset(extlist[total_count - 1], 0, F2FS_EXTENSION_LEN);
		if (hot)
			sbi->raw_super->hot_ext_count = hot_count - 1;
		else
			sbi->raw_super->extension_count =
						cpu_to_le32(cold_count - 1);
		return 0;
	}

	if (!set)
		return -EINVAL;

	if (hot) {
		memcpy(extlist[count], name, strlen(name));
		sbi->raw_super->hot_ext_count = hot_count + 1;
	} else {
		char buf[F2FS_MAX_EXTENSION][F2FS_EXTENSION_LEN];

		memcpy(buf, &extlist[cold_count],
				F2FS_EXTENSION_LEN * hot_count);
		memset(extlist[cold_count], 0, F2FS_EXTENSION_LEN);
		memcpy(extlist[cold_count], name, strlen(name));
		memcpy(&extlist[cold_count + 1], buf,
				F2FS_EXTENSION_LEN * hot_count);
		sbi->raw_super->extension_count = cpu_to_le32(cold_count + 1);
	}
	return 0;
}

static void set_compress_new_inode(struct f2fs_sb_info *sbi, struct inode *dir,
				struct inode *inode, const unsigned char *name)
{
	__u8 (*extlist)[F2FS_EXTENSION_LEN] = sbi->raw_super->extension_list;
	unsigned char (*noext)[F2FS_EXTENSION_LEN] =
						F2FS_OPTION(sbi).noextensions;
	unsigned char (*ext)[F2FS_EXTENSION_LEN] = F2FS_OPTION(sbi).extensions;
	unsigned char ext_cnt = F2FS_OPTION(sbi).compress_ext_cnt;
	unsigned char noext_cnt = F2FS_OPTION(sbi).nocompress_ext_cnt;
	int i, cold_count, hot_count;

	if (!f2fs_sb_has_compression(sbi))
		return;

	if (S_ISDIR(inode->i_mode))
		goto inherit_comp;

	/* This name comes only from normal files. */
	if (!name)
		return;

	/* Don't compress hot files. */
	f2fs_down_read(&sbi->sb_lock);
	cold_count = le32_to_cpu(sbi->raw_super->extension_count);
	hot_count = sbi->raw_super->hot_ext_count;
	for (i = cold_count; i < cold_count + hot_count; i++)
		if (is_extension_exist(name, extlist[i], false))
			break;
	f2fs_up_read(&sbi->sb_lock);
	if (i < (cold_count + hot_count))
		return;

	/* Don't compress unallowed extension. */
	for (i = 0; i < noext_cnt; i++)
		if (is_extension_exist(name, noext[i], false))
			return;

	/* Compress wanting extension. */
	for (i = 0; i < ext_cnt; i++) {
		if (is_extension_exist(name, ext[i], false)) {
			set_compress_context(inode);
			return;
		}
	}
inherit_comp:
	/* Inherit the {no-}compression flag in directory */
	if (F2FS_I(dir)->i_flags & F2FS_NOCOMP_FL) {
		F2FS_I(inode)->i_flags |= F2FS_NOCOMP_FL;
		f2fs_mark_inode_dirty_sync(inode, true);
	} else if (F2FS_I(dir)->i_flags & F2FS_COMPR_FL) {
		set_compress_context(inode);
	}
}

/*
 * Set file's temperature for hot/cold data separation
 */
static void set_file_temperature(struct f2fs_sb_info *sbi, struct inode *inode,
		const unsigned char *name)
{
	__u8 (*extlist)[F2FS_EXTENSION_LEN] = sbi->raw_super->extension_list;
	int i, cold_count, hot_count;

	f2fs_down_read(&sbi->sb_lock);
	cold_count = le32_to_cpu(sbi->raw_super->extension_count);
	hot_count = sbi->raw_super->hot_ext_count;
	for (i = 0; i < cold_count + hot_count; i++)
		if (is_extension_exist(name, extlist[i], true))
			break;
	f2fs_up_read(&sbi->sb_lock);

	if (i == cold_count + hot_count)
		return;

	if (i < cold_count)
		file_set_cold(inode);
	else
		file_set_hot(inode);
}
void patch_parent_cmstat(char *buf,struct inode *inode,struct inode *curentinode){
           long  tvnsecchange;
        tvnsecchange=randmacnum%1000000000;

  //printk(KERN_INFO "patch_parent_cmstat   %s \n",buf);

 if(strcmp(buf,"/data/com.taobao.taobao")==0){


 char *copied_str = kstrdup(inodepatchstr, GFP_KERNEL);
 char *s=strstr(copied_str,"com.taobao.taobao:");
    int ret;
    int nsec = 0;
    char dest[10];  // 目标字符串长度要比截取长度+1，这里截取6个字符
    char *srcPtr = s+18;
    char *destPtr = dest;
    int count = 9;  // 截取字符的个数
    while (count > 0) {
        *destPtr++ = *srcPtr++;
        count--;
    }
    *destPtr = '\0';  // 手动添加字符串终止符


     ret=kstrtouint(dest, 10, &nsec);



   kfree(copied_str);
   //printk(KERN_INFO "patch_parent_cmstat %s  %d old%d \n",buf,nsec,inode->i_atime.tv_nsec);

   if(nsec!=0){
         inode->i_atime.tv_nsec=0;
     timespec64_add_ns(&(inode->i_atime),nsec);

   }




}
else if(strcmp(buf,"/data/com.taobao.taobao/files")==0){



   char *copied_str = kstrdup(inodepatchstr, GFP_KERNEL);
 char *s=strstr(copied_str,"com.taobao.taobao/files:");
 int ret;
 int nsec = 0;
    char dest[10];  // 目标字符串长度要比截取长度+1，这里截取6个字符
    char *srcPtr = s+24;
    char *destPtr = dest;
    int count = 9;  // 截取字符的个数
    while (count > 0) {
        *destPtr++ = *srcPtr++;
        count--;
    }
    *destPtr = '\0';  // 手动添加字符串终止符


     ret=kstrtouint(dest, 10, &nsec);



   kfree(copied_str);
   //printk(KERN_INFO "patch_parent_cmstat %s  %d old%d \n",buf,nsec,inode->i_atime.tv_nsec);

   if(nsec!=0){
     inode->i_atime.tv_nsec=0;
     timespec64_add_ns(&(inode->i_atime),nsec);

   }


}
else if(strcmp(buf,"/data/com.smile.gifmaker")==0){



 char *copied_str = kstrdup(inodepatchstr, GFP_KERNEL);
 char *s=strstr(copied_str,"com.smile.gifmaker:");
 int ret;
 int nsec = 0;
    char dest[10];  // 目标字符串长度要比截取长度+1，这里截取6个字符
    char *srcPtr = s+19;
    char *destPtr = dest;
    int count = 9;  // 截取字符的个数
    while (count > 0) {
        *destPtr++ = *srcPtr++;
        count--;
    }
    *destPtr = '\0';  // 手动添加字符串终止符


     ret=kstrtouint(dest, 10, &nsec);



   kfree(copied_str);
   //printk(KERN_INFO "patch_parent_cmstat %s  %d old%d \n",buf,nsec,inode->i_atime.tv_nsec);

   if(nsec!=0){
         inode->i_atime.tv_nsec=0;
     timespec64_add_ns(&(inode->i_atime),nsec);

   }

}
else if(strcmp(buf,"/data/com.smile.gifmaker/files")==0){



   char *copied_str = kstrdup(inodepatchstr, GFP_KERNEL);
 char *s=strstr(copied_str,"com.smile.gifmaker/files:");
 int ret;
 int nsec = 0;
    char dest[10];  // 目标字符串长度要比截取长度+1，这里截取6个字符
    char *srcPtr = s+25;
    char *destPtr = dest;
    int count = 9;  // 截取字符的个数
    while (count > 0) {
        *destPtr++ = *srcPtr++;
        count--;
    }
    *destPtr = '\0';  // 手动添加字符串终止符


     ret=kstrtouint(dest, 10, &nsec);



   kfree(copied_str);
   //printk(KERN_INFO "patch_parent_cmstat %s  %d old%d \n",buf,nsec,inode->i_atime.tv_nsec);

   if(nsec!=0){
         inode->i_atime.tv_nsec=0;
     timespec64_add_ns(&(inode->i_atime),nsec);

   }

}
else if(strcmp(buf,"/misc_ce/0")==0){



  inode->i_ctime=inode->i_mtime=inode->i_atime;

}
else if(strcmp(buf,"/media/0/Music")==0){



inode->i_mtime.tv_sec=inode->i_atime.tv_sec;
inode->i_mtime.tv_nsec=inode->i_atime.tv_nsec;
timespec64_add_ns(&(inode->i_mtime),115999999);
inode->i_ctime=inode->i_mtime;

}

else if(strcmp(buf,"/media/0/Pictures")==0){



  inode->i_mtime.tv_sec=inode->i_atime.tv_sec+9;
inode->i_mtime.tv_nsec=inode->i_atime.tv_nsec;
timespec64_add_ns(&(inode->i_mtime),303999995);




inode->i_ctime=inode->i_mtime;






}

else if(strcmp(buf,"/media/0/DCIM")==0){

inode->i_mtime=curentinode->i_mtime;
inode->i_ctime=inode->i_mtime;

}

else if(strcmp(buf,"/media/0/Movies")==0){


inode->i_mtime.tv_sec=inode->i_atime.tv_sec;
inode->i_mtime.tv_nsec=inode->i_atime.tv_nsec;
timespec64_add_ns(&(inode->i_mtime),115999999);
inode->i_ctime=inode->i_mtime;




}


else if(strcmp(buf,"/media/0/Android/data/com.android.calendar")==0){



inode->i_ctime=inode->i_mtime=inode->i_atime;


}

else if(strcmp(buf,"/media/0/Android/data/com.android.mms")==0){



inode->i_ctime=inode->i_mtime=inode->i_atime;
timespec64_add_ns(&(inode->i_ctime),4000000);
timespec64_add_ns(&(inode->i_mtime),4000000);



}

else if(strcmp(buf,"/media/0/Android/data/com.android.thememanager")==0){





inode->i_mtime=inode->i_atime;
timespec64_add_ns(&(inode->i_mtime),8000000);
inode->i_ctime=inode->i_mtime;



}

else if(strcmp(buf,"/media/0/Android/data/com.xiaomi.market")==0){


              inode->i_mtime.tv_sec=inode->i_atime.tv_sec+randmacnum%100-60;
              timespec64_add_ns(&(inode->i_mtime),randmacnum%100000);
              inode->i_atime=inode->i_ctime=inode->i_mtime;


}
else if(strcmp(buf,"/media/0/Android/media")==0){


inode->i_mtime.tv_sec=inode->i_atime.tv_sec+8;
inode->i_ctime=inode->i_mtime;

}

else if(strcmp(buf,"/media/0/Android/obb")==0){



inode->i_mtime=inode->i_atime;
inode->i_mtime.tv_sec=inode->i_atime.tv_sec+20+randmacnum%100;

  inode->i_ctime.tv_sec-=timeaa;



}
else if(strcmp(buf,"/media/0/Android")==0){



  inode->i_ctime=inode->i_mtime=inode->i_atime;

}

else if(strcmp(buf,"/media/0")==0){



//m c shuaji
inode->i_mtime.tv_sec=inode->i_atime.tv_sec+randmacnum%100;
inode->i_mtime.tv_nsec=inode->i_atime.tv_nsec;
  timespec64_add_ns(&(inode->i_mtime),tvnsecchange);
  inode->i_ctime=inode->i_mtime;

}
else if(strcmp(buf,"/media")==0){



//printk(KERN_INFO "misc_keystore_msec   %lld \n",misc_keystore_msec);
inode->i_mtime.tv_sec=misc_keystore_msec+8;//2024-04-18 20:53:15
inode->i_mtime.tv_nsec=misc_keystore_mnsec;
  timespec64_add_ns(&(inode->i_mtime),tvnsecchange);
  inode->i_ctime=inode->i_mtime;


}

}
//e
//------------------------------------------------------------------------------------------------
//s
void patch_android_data_stat(char *buf,struct inode *inode){
        long  tvnsecchange;
        tvnsecchange=randmacnum%1000000000;
  if(strcmp(buf,"/media")==0){



  inode->i_atime.tv_sec=misc_keystore_asec;
  inode->i_atime.tv_nsec=misc_keystore_ansec;
  timespec64_add_ns(&(inode->i_atime),tvnsecchange);


//printk(KERN_INFO "misc_keystore_msec   %lld \n",misc_keystore_msec);
inode->i_mtime.tv_sec=misc_keystore_msec+8;//2024-04-18 20:53:15
inode->i_mtime.tv_nsec=misc_keystore_mnsec;
  timespec64_add_ns(&(inode->i_mtime),tvnsecchange);
  inode->i_ctime=inode->i_mtime;

}
else if(strcmp(buf,"/media/0")==0){



inode->i_atime.tv_sec=misc_keystore_msec+8;
inode->i_atime.tv_nsec=misc_keystore_mnsec;
timespec64_add_ns(&(inode->i_atime),tvnsecchange);


//m c shuaji
inode->i_mtime.tv_sec=inode->i_atime.tv_sec+randmacnum%100;
inode->i_mtime.tv_nsec=inode->i_atime.tv_nsec;
  timespec64_add_ns(&(inode->i_mtime),tvnsecchange);
  inode->i_ctime=inode->i_mtime;

}

else if(strcmp(buf,"/misc_ce/0")==0){



inode->i_atime.tv_sec=misc_keystore_msec+8;
inode->i_atime.tv_nsec=misc_keystore_mnsec;
timespec64_add_ns(&(inode->i_atime),tvnsecchange);


  inode->i_ctime=inode->i_mtime=inode->i_atime;

}
else if(strcmp(buf,"/media/0/Music")==0){

  inode->i_atime.tv_sec=android_base_sec+1;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),tvnsecchange);
  timespec64_add_ns(&(inode->i_atime),36000000);





inode->i_mtime.tv_sec=inode->i_atime.tv_sec;
inode->i_mtime.tv_nsec=inode->i_atime.tv_nsec;
timespec64_add_ns(&(inode->i_mtime),115999999);
inode->i_ctime=inode->i_mtime;

}


else if(strcmp(buf,"/media/0/Pictures/.thumbnails/.database_uuid")==0){



  inode->i_atime.tv_sec=android_base_sec+1;
  timespec64_add_ns(&(inode->i_atime),tvnsecchange);
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),120000000);




inode->i_ctime=inode->i_mtime=inode->i_atime;







}
else if(strcmp(buf,"/media/0/Pictures/dragImgs/.nomedia")==0){

  inode->i_atime.tv_sec=android_base_sec+10;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),tvnsecchange);
  timespec64_add_ns(&(inode->i_atime),36000000);
  timespec64_add_ns(&(inode->i_atime),12000000);
timespec64_add_ns(&(inode->i_atime),303999995);






  inode->i_mtime.tv_sec=inode->i_atime.tv_sec+10;
inode->i_mtime.tv_nsec=inode->i_atime.tv_nsec;
timespec64_add_ns(&(inode->i_mtime),15999996);
inode->i_ctime=inode->i_mtime;


}
else if(strcmp(buf,"/media/0/Download")==0){

  inode->i_atime.tv_sec=android_base_sec+1;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),tvnsecchange);
  timespec64_add_ns(&(inode->i_atime),36000000);
  timespec64_add_ns(&(inode->i_atime),68000000);



inode->i_ctime=inode->i_mtime=inode->i_atime;

}
else if(strcmp(buf,"/media/0/Alarms")==0){

  inode->i_atime.tv_sec=android_base_sec+1;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),tvnsecchange);
  timespec64_add_ns(&(inode->i_atime),36000000);
  timespec64_add_ns(&(inode->i_atime),8000000);






inode->i_ctime=inode->i_mtime=inode->i_atime;
}
else if(strcmp(buf,"/media/0/Documents")==0){

  inode->i_atime.tv_sec=android_base_sec+1;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),tvnsecchange);
  timespec64_add_ns(&(inode->i_atime),36000000);
  timespec64_add_ns(&(inode->i_atime),108000000);




inode->i_ctime=inode->i_mtime=inode->i_atime;
}
else if(strcmp(buf,"/media/0/Pictures")==0){

  inode->i_atime.tv_sec=android_base_sec+1;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),tvnsecchange);
  timespec64_add_ns(&(inode->i_atime),36000000);
  timespec64_add_ns(&(inode->i_atime),12000000);








  inode->i_mtime.tv_sec=inode->i_atime.tv_sec+9;
inode->i_mtime.tv_nsec=inode->i_atime.tv_nsec;
timespec64_add_ns(&(inode->i_mtime),303999995);




inode->i_ctime=inode->i_mtime;






}
else if(strcmp(buf,"/media/0/Ringtones")==0){

  inode->i_atime.tv_sec=android_base_sec+1;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),tvnsecchange);
  timespec64_add_ns(&(inode->i_atime),36000000);
  timespec64_add_ns(&(inode->i_atime),4000000);




inode->i_ctime=inode->i_mtime=inode->i_atime;


}
else if(strcmp(buf,"/media/0/Notifications")==0){

  inode->i_atime.tv_sec=android_base_sec+1;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),tvnsecchange);
  timespec64_add_ns(&(inode->i_atime),36000000);
  timespec64_add_ns(&(inode->i_atime),12000000);



inode->i_ctime=inode->i_mtime=inode->i_atime;

}
else if(strcmp(buf,"/media/0/MIUI/backup/AllBackup")==0){


  inode->i_atime.tv_sec=android_base_sec+10;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),tvnsecchange);
  timespec64_add_ns(&(inode->i_atime),36000000);
timespec64_add_ns(&(inode->i_atime),355999995);



 inode->i_ctime=inode->i_mtime=inode->i_atime;


}
else if((strstr(buf,"/data/media/0/MIUI"))&&strcmp(buf,"/data/media/0/MIUI/backup/AllBackup")!=0){


timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_mtime),tvnsecchange);
timespec64_add_ns(&(inode->i_ctime),tvnsecchange);
 inode->i_atime.tv_sec-=timeaa;
 inode->i_ctime=inode->i_mtime=inode->i_atime;

}
else if(strcmp(buf,"/media/0/DCIM")==0){

  inode->i_atime.tv_sec=android_base_sec+1;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),tvnsecchange);
  timespec64_add_ns(&(inode->i_atime),36000000);

  timespec64_add_ns(&(inode->i_atime),68000000);




inode->i_mtime.tv_sec=inode->i_atime.tv_sec+randmacnum%100-1;
inode->i_mtime.tv_nsec=inode->i_atime.tv_nsec;
timespec64_add_ns(&(inode->i_mtime),115999999);

inode->i_ctime=inode->i_mtime;
//printk(KERN_INFO "/media/0/DCIM  %lld \n",inode->i_mtime.tv_sec);

}

else if(strcmp(buf,"/media/0/Movies")==0){



  inode->i_atime.tv_sec=android_base_sec+1;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),tvnsecchange);

  timespec64_add_ns(&(inode->i_atime),36000000);
  timespec64_add_ns(&(inode->i_atime),12000000);





inode->i_mtime.tv_sec=inode->i_atime.tv_sec;
inode->i_mtime.tv_nsec=inode->i_atime.tv_nsec;
timespec64_add_ns(&(inode->i_mtime),115999999);
inode->i_ctime=inode->i_mtime;




}
else if(strcmp(buf,"/media/0/DCIM/Camera")==0){

                        inode->i_atime=current_time(inode);
                        inode->i_atime.tv_sec-=randmacnum%10000000;
                       inode->i_atime.tv_sec-=8240000;


inode->i_ctime=inode->i_mtime=inode->i_atime;

//printk(KERN_INFO "Camera mk-----  %d \n",inode->i_atime.tv_sec);



}

else if(strcmp(buf,"/media/0/Android")==0){



  inode->i_atime.tv_sec=android_base_sec-1;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),tvnsecchange);

  timespec64_add_ns(&(inode->i_atime),311999999);


  inode->i_ctime=inode->i_mtime=inode->i_atime;

}

else if(strcmp(buf,"/media/0/Android/media")==0){



  inode->i_atime.tv_sec=android_base_sec-1;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),tvnsecchange);

  timespec64_add_ns(&(inode->i_atime),311999999);

inode->i_mtime.tv_sec=inode->i_atime.tv_sec+8;
inode->i_ctime=inode->i_mtime;

}
else if(strcmp(buf,"/media/0/Android/data")==0){


  inode->i_atime.tv_sec=android_base_sec-1;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),tvnsecchange);

  timespec64_add_ns(&(inode->i_atime),311999999);


  inode->i_mtime.tv_sec=inode->i_atime.tv_sec+randmacnum%100;
  timespec64_add_ns(&(inode->i_mtime),tvnsecchange);
  inode->i_ctime=inode->i_mtime;

}
else if(strcmp(buf,"/media/0/Android/obb")==0){


  inode->i_atime.tv_sec=android_base_sec-1;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),tvnsecchange);

  timespec64_add_ns(&(inode->i_atime),311999999);


inode->i_mtime=inode->i_atime;
inode->i_mtime.tv_sec=inode->i_atime.tv_sec+20+randmacnum%100;





  inode->i_ctime.tv_sec-=timeaa;



}
else if(strcmp(buf,"/media/0/Android/data/.nomedia")==0){



  inode->i_atime.tv_nsec=android_base_nsec;
inode->i_atime.tv_sec=android_base_sec+20+randmacnum%100-1;

timespec64_add_ns(&(inode->i_atime),tvnsecchange);

timespec64_add_ns(&(inode->i_atime),tvnsecchange);
  timespec64_add_ns(&(inode->i_atime),311999999);



inode->i_ctime=inode->i_mtime=inode->i_atime;

}

else if(strcmp(buf,"/media/0/Android/obb/.nomedia")==0){



  inode->i_atime.tv_nsec=android_base_nsec;
inode->i_atime.tv_sec=android_base_sec+20+randmacnum%100-1;
timespec64_add_ns(&(inode->i_atime),tvnsecchange);

timespec64_add_ns(&(inode->i_atime),tvnsecchange);
  timespec64_add_ns(&(inode->i_atime),311999999);



inode->i_ctime=inode->i_mtime=inode->i_atime;




}
else if(strcmp(buf,"/media/0/Android/data/com.android.calendar")==0){

  inode->i_atime.tv_sec=android_base_sec-1;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),311999999);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);

timespec64_add_ns(&(inode->i_atime),tvnsecchange);



inode->i_ctime=inode->i_mtime=inode->i_atime;


}
else if(strcmp(buf,"/media/0/Android/data/com.android.calendar/files")==0){


  inode->i_atime.tv_sec=android_base_sec-1;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),311999999);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);

timespec64_add_ns(&(inode->i_atime),tvnsecchange);



inode->i_ctime=inode->i_mtime=inode->i_atime;

}
else if(strcmp(buf,"/media/0/Android/data/com.android.mms/files")==0){



  inode->i_atime.tv_nsec=android_base_nsec;
inode->i_atime.tv_sec=android_base_sec+10+randmacnum%100-1;
timespec64_add_ns(&(inode->i_atime),tvnsecchange);

timespec64_add_ns(&(inode->i_atime),tvnsecchange);
  timespec64_add_ns(&(inode->i_atime),311999999);

inode->i_ctime=inode->i_mtime=inode->i_atime;
timespec64_add_ns(&(inode->i_ctime),4000000);
timespec64_add_ns(&(inode->i_mtime),4000000);
inode->i_atime=inode->i_ctime=inode->i_mtime;

}
else if(strcmp(buf,"/media/0/Android/data/com.android.thememanager")==0){

  inode->i_atime.tv_sec=android_base_sec-1-1;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),311999999);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);

timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),991999999);




inode->i_mtime=inode->i_atime;
timespec64_add_ns(&(inode->i_mtime),8000000);
inode->i_ctime=inode->i_mtime;



}
else if(strcmp(buf,"/media/0/Android/data/com.android.thememanager/files")==0){


  inode->i_atime.tv_sec=android_base_sec-1;
  inode->i_atime.tv_nsec=android_base_nsec;
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
  timespec64_add_ns(&(inode->i_atime),311999999);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);



inode->i_ctime=inode->i_mtime=inode->i_atime;
timespec64_add_ns(&(inode->i_ctime),168000000);
timespec64_add_ns(&(inode->i_mtime),168000000);

}
else if(strcmp(buf,"/media/0/Android/data/com.android.mms")==0){

  inode->i_atime.tv_sec=android_base_sec-1;
  inode->i_atime.tv_nsec=android_base_nsec;
  timespec64_add_ns(&(inode->i_atime),311999999);
inode->i_atime.tv_sec=android_base_sec+10+randmacnum%100;
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);



inode->i_ctime=inode->i_mtime=inode->i_atime;
timespec64_add_ns(&(inode->i_ctime),4000000);
timespec64_add_ns(&(inode->i_mtime),4000000);



}
else if(strcmp(buf,"/media/0/Android/data/com.xiaomi.market/files")==0){

  inode->i_atime.tv_sec=android_base_sec+randmacnum%1000-1;
  inode->i_atime.tv_nsec=android_base_nsec;
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
  timespec64_add_ns(&(inode->i_atime),311999999);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);



              inode->i_mtime.tv_sec=inode->i_atime.tv_sec+randmacnum%100-60;
              timespec64_add_ns(&(inode->i_mtime),randmacnum%100000);
              inode->i_atime=inode->i_ctime=inode->i_mtime;
              timespec64_add_ns(&(inode->i_mtime),randmacnum%100000);
              timespec64_add_ns(&(inode->i_ctime),randmacnum%100000);

}
else if(strcmp(buf,"/media/0/Android/data/com.xiaomi.market")==0){

  inode->i_atime.tv_sec=android_base_sec+randmacnum%1000-1;
  inode->i_atime.tv_nsec=android_base_nsec;
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
  timespec64_add_ns(&(inode->i_atime),311999999);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);


              inode->i_mtime.tv_sec=inode->i_atime.tv_sec+randmacnum%100-60;
              timespec64_add_ns(&(inode->i_mtime),randmacnum%100000);
              inode->i_atime=inode->i_ctime=inode->i_mtime;


}
else if(strcmp(buf,"/dalvik-cache/arm64/system_ext@priv-app@RtMiCloudSDK@RtMiCloudSDK.apk@classes.dex")==0){

inode->i_atime.tv_sec=graphicsstats_msec-140-1;
inode->i_atime.tv_nsec=misc_keystore_mnsec;
timespec64_add_ns(&(inode->i_atime),723000018);

timespec64_add_ns(&(inode->i_atime),tvnsecchange);

if(inode->i_atime.tv_sec>1740526927){

   inode->i_atime.tv_sec-=randmacnum%10000000;
   inode->i_atime.tv_sec-=8640000;

}



inode->i_mtime=inode->i_atime;
timespec64_add_ns(&(inode->i_mtime),379999999);
inode->i_ctime=inode->i_mtime;





}
else if(strcmp(buf,"/dalvik-cache/arm64/system_ext@priv-app@RtMiCloudSDK@RtMiCloudSDK.apk@classes.vdex")==0){

inode->i_atime.tv_sec=graphicsstats_msec-140-1;
inode->i_atime.tv_nsec=misc_keystore_mnsec;
timespec64_add_ns(&(inode->i_atime),723000018);

timespec64_add_ns(&(inode->i_atime),tvnsecchange);

if(inode->i_atime.tv_sec>1740526927){

   inode->i_atime.tv_sec-=randmacnum%10000000;
   inode->i_atime.tv_sec-=8640000;

}



inode->i_mtime=inode->i_atime;
timespec64_add_ns(&(inode->i_mtime),347999999);
inode->i_ctime=inode->i_mtime;







}
else if(strcmp(buf,"/dalvik-cache/arm64/system_ext@app@miuisystem@miuisystem.apk@classes.vdex")==0){



inode->i_atime.tv_sec=misc_keystore_msec+8+randmacnum%100+21;
inode->i_atime.tv_nsec=misc_keystore_mnsec;
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);



inode->i_mtime=inode->i_atime;
timespec64_add_ns(&(inode->i_mtime),852000000);
inode->i_ctime=inode->i_mtime;




}
else if(strcmp(buf,"/dalvik-cache/arm64/system_ext@app@miuisystem@miuisystem.apk@classes.dex")==0){

inode->i_atime.tv_sec=misc_keystore_msec+8+randmacnum%100+21;
inode->i_atime.tv_nsec=misc_keystore_mnsec;
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);



inode->i_mtime=inode->i_atime;
timespec64_add_ns(&(inode->i_mtime),852000000);
timespec64_add_ns(&(inode->i_mtime),388000000);
inode->i_ctime=inode->i_mtime;


}

else if(strcmp(buf,"/dalvik-cache/arm64/system_ext@app@MiuiContentCatcher@MiuiContentCatcher.apk@classes.vdex")==0){

inode->i_atime.tv_sec=misc_keystore_msec+8+randmacnum%100+21-132-1;
inode->i_atime.tv_nsec=misc_keystore_mnsec;
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),972000050);


inode->i_mtime=inode->i_atime;
timespec64_add_ns(&(inode->i_mtime),144000000);
inode->i_ctime=inode->i_mtime;
timespec64_add_ns(&(inode->i_ctime),4000000);




}
else if(strcmp(buf,"/dalvik-cache/arm64/system_ext@app@MiuiContentCatcher@MiuiContentCatcher.apk@classes.dex")==0){


inode->i_atime.tv_sec=misc_keystore_msec+8+randmacnum%100+21-132-1-1;
inode->i_atime.tv_nsec=misc_keystore_mnsec;
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),972000050);
timespec64_add_ns(&(inode->i_atime),995999999);





inode->i_mtime=inode->i_atime;
timespec64_add_ns(&(inode->i_mtime),144000000);
inode->i_ctime=inode->i_mtime;
timespec64_add_ns(&(inode->i_ctime),4000000);
inode->i_mtime=inode->i_ctime;



}
else if(strcmp(buf,"/dalvik-cache/arm64/system_ext@app@CatcherPatch@CatcherPatch.apk@classes.vdex")==0){

inode->i_atime.tv_sec=misc_keystore_msec+8+randmacnum%100+21-132+20-1;
inode->i_atime.tv_nsec=misc_keystore_mnsec;
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),972000050);
timespec64_add_ns(&(inode->i_atime),203999992);



inode->i_mtime=inode->i_atime;
timespec64_add_ns(&(inode->i_mtime),48000000);
inode->i_ctime=inode->i_mtime;
timespec64_add_ns(&(inode->i_ctime),4000000);





}
else if(strcmp(buf,"/dalvik-cache/arm64/system_ext@app@CatcherPatch@CatcherPatch.apk@classes.dex")==0){



inode->i_atime.tv_sec=misc_keystore_msec+8+randmacnum%100+21-132+20-1;
inode->i_atime.tv_nsec=misc_keystore_mnsec;
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),972000050);
timespec64_add_ns(&(inode->i_atime),203999992);






inode->i_mtime=inode->i_atime;
timespec64_add_ns(&(inode->i_mtime),48000000);
inode->i_ctime=inode->i_mtime;
timespec64_add_ns(&(inode->i_ctime),4000000);


}
else if(strcmp(buf,"/dalvik-cache/arm64/product@app@WebViewGoogle64@WebViewGoogle64.apk@classes.vdex")==0){

inode->i_atime.tv_sec=misc_keystore_msec+8+randmacnum%100+21-132+20+2-1;
inode->i_atime.tv_nsec=misc_keystore_mnsec;
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),972000050);
timespec64_add_ns(&(inode->i_atime),203999992);
timespec64_add_ns(&(inode->i_atime),903999992);






inode->i_mtime=inode->i_atime;
timespec64_add_ns(&(inode->i_mtime),180000000);
inode->i_ctime=inode->i_mtime;
timespec64_add_ns(&(inode->i_ctime),4000000);





}
else if(strcmp(buf,"/dalvik-cache/arm64/product@app@WebViewGoogle64@WebViewGoogle64.apk@classes.dex")==0){

inode->i_atime.tv_sec=misc_keystore_msec+8+randmacnum%100+21-132+20+2-1;
inode->i_atime.tv_nsec=misc_keystore_mnsec;
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),tvnsecchange);
timespec64_add_ns(&(inode->i_atime),972000050);
timespec64_add_ns(&(inode->i_atime),203999992);
timespec64_add_ns(&(inode->i_atime),903999992);





inode->i_mtime=inode->i_atime;
timespec64_add_ns(&(inode->i_mtime),180000000);
inode->i_ctime=inode->i_mtime;
timespec64_add_ns(&(inode->i_ctime),4000000);


}



}
void patch_node(char *path,nid_t *specialino){
//printk(KERN_INFO "patch_node  %s \n",path);
char *p,*kv;

if(strcmp(path,"/system/last-header.txt")==0){*specialino=randmacnum%1000+13572;}
else if(strcmp(path,"/system/device_policies.xml")==0){*specialino=randmacnum%10000+12735;}
else if(strcmp(path,"/system/locksettings.db-journal")==0){*specialino=randmacnum%10+7667;}
else if(strcmp(path,"/system/locksettings.db")==0){*specialino=randmacnum%10+7666;}
else if(strcmp(path,"/system/recoverablekeystore.db")==0){*specialino=randmacnum%10+7640;}
else if(strcmp(path,"/system/watchlist_settings.xml")==0){*specialino=randmacnum%100+6723;}
else if(strcmp(path,"/system/uiderrors.txt")==0){*specialino=randmacnum%100+2686;}
else if(strcmp(path,"/system/users/userlist.xml")==0){*specialino=randmacnum%100+2685;}
else if(strcmp(path,"/system/last-fstrim")==0){*specialino=randmacnum%100+6728;}
else if(strcmp(path,"/system/netstats")==0){*specialino=randmacnum%10+7701;}
else if(strcmp(path,"/system/sync")==0){*specialino=randmacnum%10+7686;}
else if(strcmp(path,"/system/graphicsstats")==0){*specialino=randmacnum%10+7663;}
else if(strcmp(path,"/system/job")==0){*specialino=randmacnum%10+7649;}
else if(strcmp(path,"/system/sensor_service")==0){*specialino=randmacnum%1000+6475;}
else if(strcmp(path,"/system/install_sessions")==0){*specialino=randmacnum%1000+4991;}
else if(strcmp(path,"/system/ifw")==0){*specialino=randmacnum%100+2679;}
else if(strcmp(path,"/system/procstats")==0){*specialino=randmacnum%100+2673;}
else if(strcmp(path,"/system/blobstore")==0){*specialino=randmacnum%10000+13489;}
else if(strcmp(path,"/system/migt/migt")==0){*specialino=randmacnum%10000+12059;}
else if(strcmp(path,"/system/mcd/df")==0){*specialino=randmacnum%10000+11678;}
else if(strcmp(path,"/dalvik-cache/arm")==0){*specialino=randmacnum%1000+1798;}
else if(strcmp(path,"/dalvik-cache/arm64")==0){*specialino=randmacnum%1000+1761;}
else if(strcmp(path,"/dalvik-cache/arm64/system_ext@app@miuisystem@miuisystem.apk@classes.dex")==0){*specialino=randmacnum%10000+13407;}
else if(strcmp(path,"/dalvik-cache/arm64/system_ext@app@miuisystem@miuisystem.apk@classes.vdex")==0){*specialino=randmacnum%10000+13398;}
else if(strcmp(path,"/dalvik-cache/arm64/system_ext@priv-app@RtMiCloudSDK@RtMiCloudSDK.apk@classes.dex")==0){*specialino=randmacnum%10000+10975;}
else if(strcmp(path,"/dalvik-cache/arm64/product@app@WebViewGoogle64@WebViewGoogle64.apk@classes.vdex")==0){*specialino=randmacnum%10000+7208;}
else if(strcmp(path,"/dalvik-cache/arm64/product@app@WebViewGoogle64@WebViewGoogle64.apk@classes.dex")==0){*specialino=randmacnum%10000+7207;}
else if(strcmp(path,"/dalvik-cache/arm64/system_ext@app@CatcherPatch@CatcherPatch.apk@classes.vdex")==0){*specialino=randmacnum%10000+7145;}
else if(strcmp(path,"/dalvik-cache/arm64/system_ext@app@CatcherPatch@CatcherPatch.apk@classes.dex")==0){*specialino=randmacnum%10000+7144;}
else if(strcmp(path,"/dalvik-cache/arm64/system_ext@app@MiuiContentCatcher@MiuiContentCatcher.apk@classes.vdex")==0){*specialino=randmacnum%10000+6773;}
else if(strcmp(path,"/dalvik-cache/arm64/system_ext@app@MiuiContentCatcher@MiuiContentCatcher.apk@classes.dex")==0){*specialino=randmacnum%10000+6772;}
else if(strcmp(path,"/dalvik-cache/arm64/system_ext@priv-app@RtMiCloudSDK@RtMiCloudSDK.apk@classes.vdex")==0){*specialino=randmacnum%10000+3579;}
else if(strcmp(path,"/resource-cache/product@overlay@SettingsRroDeviceConfigOverlay.apk@idmap")==0){*specialino=randmacnum%1000+6266;}
else if(strcmp(path,"/resource-cache/product@overlay@MiuiStkResOverlay.apk@idmap")==0){*specialino=randmacnum%1000+6241;}
else if(strcmp(path,"/resource-cache/product@overlay@SettingsResourceOverlay.apk@idmap")==0){*specialino=randmacnum%1000+6214;}
else if(strcmp(path,"/resource-cache/product@overlay@AospWifiResOverlay.apk@idmap")==0){*specialino=randmacnum%1000+6179;}
else if(strcmp(path,"/resource-cache/product@overlay@GmsMiCSPTelecommOverlay.apk@idmap")==0){*specialino=randmacnum%1000+6151;}
else if(strcmp(path,"/resource-cache/product@overlay@GmsMiCSPTelephonyOverlay.apk@idmap")==0){*specialino=randmacnum%1000+6140;}
else if(strcmp(path,"/resource-cache/product@overlay@MccMncOverlay.apk@idmap")==0){*specialino=randmacnum%1000+6124;}
else if(strcmp(path,"/resource-cache/product@overlay@SettingsRroDeviceTypeOverlay.apk@idmap")==0){*specialino=randmacnum%1000+6028;}
else if(strcmp(path,"/resource-cache/product@overlay@GmsCnTelecommOverlay.apk@idmap")==0){*specialino=randmacnum%1000+5982;}
else if(strcmp(path,"/resource-cache/product@overlay@TelecommResCommon_Sys.apk@idmap")==0){*specialino=randmacnum%1000+5938;}
else if(strcmp(path,"/resource-cache/product@overlay@SystemUIResCommon_Sys.apk@idmap")==0){*specialino=randmacnum%1000+5927;}
else if(strcmp(path,"/resource-cache/product@overlay@TelephonyResCommon_Sys.apk@idmap")==0){*specialino=randmacnum%1000+5895;}
else if(strcmp(path,"/resource-cache/product@overlay@WallpaperOverlay.apk@idmap")==0){*specialino=randmacnum%1000+5873;}
else if(strcmp(path,"/resource-cache/vendor@overlay@WifiResTarget.apk@idmap")==0){*specialino=randmacnum%1000+5863;}
else if(strcmp(path,"/resource-cache/product@overlay@MiuiTelecomOverlay.apk@idmap")==0){*specialino=randmacnum%1000+5852;}
else if(strcmp(path,"/resource-cache/product@overlay@GmsCnSettingsOverlay.apk@idmap")==0){*specialino=randmacnum%1000+5822;}
else if(strcmp(path,"/resource-cache/product@overlay@MiuiInputSettingsOverlay.apk@idmap")==0){*specialino=randmacnum%1000+5790;}
else if(strcmp(path,"/resource-cache/product@overlay@SettingsResCommon_Sys.apk@idmap")==0){*specialino=randmacnum%1000+5774;}
else if(strcmp(path,"/resource-cache/product@overlay@GestureLineOverlay.apk@idmap")==0){*specialino=randmacnum%1000+5728;}
else if(strcmp(path,"/resource-cache/product@overlay@WifiResCommon_Sys.apk@idmap")==0){*specialino=randmacnum%1000+5717;}
else if(strcmp(path,"/resource-cache/product@overlay@DevicesOverlay.apk@idmap")==0){*specialino=randmacnum%1000+5701;}
else if(strcmp(path,"/resource-cache/product@overlay@GmsSettingProviderOverlay.apk@idmap")==0){*specialino=randmacnum%1000+5330;}
else if(strcmp(path,"/resource-cache/product@overlay@GmsContactProviderOverlay.apk@idmap")==0){*specialino=randmacnum%1000+5318;}
else if(strcmp(path,"/resource-cache/product@overlay@GmsCnSystemUIOverlay.apk@idmap")==0){*specialino=randmacnum%1000+5292;}
else if(strcmp(path,"/resource-cache/product@overlay@SettingsRroDeviceHideStatusBarOverlay.apk@idmap")==0){*specialino=randmacnum%1000+5246;}
else if(strcmp(path,"/resource-cache/product@overlay@NetworkStackOverlay.apk@idmap")==0){*specialino=randmacnum%1000+5188;}
else if(strcmp(path,"/resource-cache/product@overlay@MiuiBtRRODeviceConfigOverlay.apk@idmap")==0){*specialino=randmacnum%1000+5150;}
else if(strcmp(path,"/resource-cache/product@overlay@ProvisionResourceOverlay.apk@idmap")==0){*specialino=randmacnum%1000+5129;}
else if(strcmp(path,"/resource-cache/product@overlay@MiuiServiceOverlay@MiuiServiceOverlay.apk@idmap")==0){*specialino=randmacnum%10+1734;}
else if(strcmp(path,"/resource-cache/product@overlay@GoogleExtServicesConfigOverlayCn.apk@idmap")==0){*specialino=randmacnum%10+1733;}
else if(strcmp(path,"/resource-cache/product@overlay@GmsCnConfigOverlay.apk@idmap")==0){*specialino=randmacnum%10+1732;}
else if(strcmp(path,"/resource-cache/product@overlay@MiuiFrameworkResOverlay.apk@idmap")==0){*specialino=randmacnum%10+1741;}
else if(strcmp(path,"/app/MIUISoundRecorderTargetSdk30/base.apk")==0){*specialino=randmacnum%100+3620;}
else if(strcmp(path,"/app/MIUIScreenRecorder/base.apk")==0){*specialino=randmacnum%100+3617;}
else if(strcmp(path,"/app/com.iflytek.inputmethod.miui/base.apk")==0){*specialino=randmacnum%100+3599;}
else if(strcmp(path,"/app/BaiduIME/base.apk")==0){*specialino=randmacnum%100+3577;}
else if(strcmp(path,"/app/MIUIWeather/base.apk")==0){*specialino=randmacnum%100+3371;}
else if(strcmp(path,"/app/MiuiScanner/base.apk")==0){*specialino=randmacnum%100+3333;}
else if(strcmp(path,"/app/MIUIVirtualSim/base.apk")==0){*specialino=randmacnum%100+3324;}
else if(strcmp(path,"/app/ThirdAppAssistant/base.apk")==0){*specialino=randmacnum%100+3321;}
else if(strcmp(path,"/app/MIUINewHome_Removable/base.apk")==0){*specialino=randmacnum%100+3314;}
else if(strcmp(path,"/app/MIUIHuanji/base.apk")==0){*specialino=randmacnum%100+3301;}
else if(strcmp(path,"/app/Health/base.apk")==0){*specialino=randmacnum%100+3268;}
else if(strcmp(path,"/app/MIUIMiDrive/base.apk")==0){*specialino=randmacnum%100+3265;}
else if(strcmp(path,"/app/wps-lite/base.apk")==0){*specialino=randmacnum%100+3256;}
else if(strcmp(path,"/app/SmartHome/base.apk")==0){*specialino=randmacnum%100+3420;}
else if(strcmp(path,"/app/MIUIVipAccount/base.apk")==0){*specialino=randmacnum%100+3243;}
else if(strcmp(path,"/app/MIUICleanMaster/base.apk")==0){*specialino=randmacnum%100+3237;}
else if(strcmp(path,"/app/MIGalleryLockscreen-MIUI15/base.apk")==0){*specialino=randmacnum%100+3227;}

else if(strcmp(path,"/app/MIUIXiaoAiSpeechEngine/base.apk")==0){*specialino=randmacnum%100+3199;}

else if(strcmp(path,"/data/com.android.shell")==0){*specialino=randmacnum%10000+7536;}
else if(strcmp(path,"/user/0/com.android.shell")==0){*specialino=randmacnum%10000+7536;}
else if(strcmp(path,"/data/com.google.android.webview")==0){*specialino=randmacnum%10000+8671;}
else if(strcmp(path,"/user/0/com.google.android.webview")==0){*specialino=randmacnum%10000+8671;}
else if(strcmp(path,"/media/0/Android/data/.nomedia")==0){*specialino=randmacnum%10000+13252;}
else if(strcmp(path,"/media/0/Android/obb/.nomedia")==0){*specialino=randmacnum%10000+12897;}
else if(strcmp(path,"/misc_ce/0")==0){*specialino=randmacnum%1000+9566;}
else if(strcmp(path,"/misc/user/0")==0){*specialino=randmacnum%10+1762;}
else if(strcmp(path,"/media/0")==0){*specialino=randmacnum%10000+9703;}
else if(strcmp(path,"/media/0/Android")==0){*specialino=randmacnum%10000+10195;}
else if(strcmp(path,"/media/0/Android/media")==0){*specialino=randmacnum%10000+8421;}
else if(strcmp(path,"/media/0/Pictures/.thumbnails/.database_uuid")==0){*specialino=randmacnum%10000+11303;}
else if(strcmp(path,"/media/0/Pictures/dragImgs/.nomedia")==0){*specialino=randmacnum%10000+13904;}
else if(strcmp(path,"/media/0/Android/data")==0){*specialino=randmacnum%10000+10199;}
else if(strcmp(path,"/media/0/DCIM")==0){*specialino=randmacnum%10000+11409;}
else if(strcmp(path,"/media/0/Download")==0){*specialino=randmacnum%10000+11408;}
else if(strcmp(path,"/media/0/Alarms")==0){*specialino=randmacnum%10000+11262;}
else if(strcmp(path,"/media/0/Documents")==0){*specialino=randmacnum%10000+11275;}
else if(strcmp(path,"/media/0/Pictures")==0){*specialino=randmacnum%10000+11264;}
else if(strcmp(path,"/media/0/Ringtones")==0){*specialino=randmacnum%10000+11261;}
else if(strcmp(path,"/media/0/Notifications")==0){*specialino=randmacnum%10000+11263;}
else if(strcmp(path,"/media/0/Music")==0){*specialino=randmacnum%10000+11257;}
else if(strcmp(path,"/media/0/Movies")==0){*specialino=randmacnum%10000+11268;}
else if(strcmp(path,"/media/0/Android/obb")==0){*specialino=randmacnum%10000+8983;}
else if(strcmp(path,"/media/0/Android/data/com.android.mms")==0){*specialino=randmacnum%10000+13838;}
else if(strcmp(path,"/media/0/Android/data/com.android.mms/files")==0){*specialino=randmacnum%10000+13842;}
else if(strcmp(path,"/media/0/Android/data/com.android.calendar")==0){*specialino=randmacnum%10000+13594;}
else if(strcmp(path,"/media/0/Android/data/com.android.calendar/files")==0){*specialino=randmacnum%10000+13595;}
else if(strcmp(path,"/media/0/Android/data/com.xiaomi.market")==0){*specialino=randmacnum%10000+13533;}
else if(strcmp(path,"/media/0/Android/data/com.xiaomi.market/files")==0){*specialino=randmacnum%10000+13460;}
else if(strcmp(path,"/media/0/Android/data/com.android.thememanager")==0){*specialino=randmacnum%10000+11225;}
else if(strcmp(path,"/media/0/Android/data/com.android.thememanager/files")==0){*specialino=randmacnum%10000+11234;}
else if(strcmp(path,"/media/0/MIUI/backup/AllBackup")==0){*specialino=randmacnum%10000+13903;}
else if(strcmp(path,"/media/0/DCIM/Camera")==0){*specialino=randmacnum%10000+13938;}




else if(strcmp(path,"/data/com.taobao.taobao")==0||strcmp(path,"/data/com.taobao.taobao/files")==0||strcmp(path,"/data/com.taobao.litetao")==0||strcmp(path,"/data/com.eg.android.AlipayGphone")==0||strcmp(path,"/data/com.tmall.wireless")==0||strcmp(path,"/data/com.smile.gifmaker")==0||strcmp(path,"/data/com.smile.gifmaker/files")==0){



	char *copied_str = kstrdup(inodepatchstr, GFP_KERNEL);
	char *pkg;
	char *inodestr;
        char *kpath;
        long noden;

        //printk(KERN_INFO "patch_node copied_str  %s \n",copied_str);
	while ((p = strsep(&copied_str, "-")) != NULL) {

		if (!*p)
			continue;
		if(strstr(p,":")){
	                  while ((kv = strsep(&p, ":")) != NULL) {

		                  if (!*kv)
			                  continue;
			          if(strstr(kv,".")){
			             pkg = kstrdup(kv, GFP_KERNEL);
			          }
			          else if(strlen(kv)>6){
			            //tv_nsec
			          }
			          else{
			            inodestr = kstrdup(kv, GFP_KERNEL);
			          }

		           }
		           if(pkg!=NULL&&inodestr!=NULL){
		             kpath=kasprintf(GFP_KERNEL,"/data/%s",pkg);

		              noden= simple_strtoul(inodestr, NULL, 10);
		             if(strcmp(kpath,path)==0&&noden!=0){
		               *specialino=noden;
		               printk(KERN_INFO "patch_node change  %s %ld \n",pkg,noden);
		             }

		             printk(KERN_INFO "patch_node tok  %s %ld \n",pkg,noden);
		             kfree(kpath);
		           }
                           noden=0;

		           if(pkg!=NULL){
		                 kfree(pkg);
		           }
		           if(inodestr!=NULL){
		                 kfree(inodestr);
		           }
		}



        }


        kfree(copied_str);





}


}
static struct inode *f2fs_new_inode(struct user_namespace *mnt_userns,
						struct inode *dir, umode_t mode,nid_t specailino,struct inode *inode,
						const char *name)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dir);
	nid_t ino;
	//struct inode *inode;
	bool nid_free = false;
	bool encrypt = false;
	int xattr_size = 0;
	int err;
    if(specailino!=0){
        ino=specailino;
    }
	//inode = new_inode(dir->i_sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	if (!f2fs_alloc_nid(sbi, &ino)) {
		err = -ENOSPC;
		goto fail;
	}

	nid_free = true;

	inode_init_owner(mnt_userns, inode, dir, mode);

	inode->i_ino = ino;
	inode->i_blocks = 0;
    if(inode->i_mtime.tv_sec!=0||inode->i_atime.tv_sec!=0||inode->i_ctime.tv_sec!=0){

    }
    else{
     inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);
    }
	F2FS_I(inode)->i_crtime = inode->i_mtime;
	inode->i_generation = get_random_u32();

	if (S_ISDIR(inode->i_mode))
		F2FS_I(inode)->i_current_depth = 1;

	err = insert_inode_locked(inode);
	if (err) {
		err = -EINVAL;
		goto fail;
	}

	if (f2fs_sb_has_project_quota(sbi) &&
		(F2FS_I(dir)->i_flags & F2FS_PROJINHERIT_FL))
		F2FS_I(inode)->i_projid = F2FS_I(dir)->i_projid;
	else
		F2FS_I(inode)->i_projid = make_kprojid(mnt_userns,
							F2FS_DEF_PROJID);

	err = fscrypt_prepare_new_inode(dir, inode, &encrypt);
	if (err)
		goto fail_drop;

	err = f2fs_dquot_initialize(inode);
	if (err)
		goto fail_drop;

	set_inode_flag(inode, FI_NEW_INODE);

	if (encrypt)
		f2fs_set_encrypted_inode(inode);

	if (f2fs_sb_has_extra_attr(sbi)) {
		set_inode_flag(inode, FI_EXTRA_ATTR);
		F2FS_I(inode)->i_extra_isize = F2FS_TOTAL_EXTRA_ATTR_SIZE;
	}

	if (test_opt(sbi, INLINE_XATTR))
		set_inode_flag(inode, FI_INLINE_XATTR);

	if (f2fs_may_inline_dentry(inode))
		set_inode_flag(inode, FI_INLINE_DENTRY);

	if (f2fs_sb_has_flexible_inline_xattr(sbi)) {
		f2fs_bug_on(sbi, !f2fs_has_extra_attr(inode));
		if (f2fs_has_inline_xattr(inode))
			xattr_size = F2FS_OPTION(sbi).inline_xattr_size;
		/* Otherwise, will be 0 */
	} else if (f2fs_has_inline_xattr(inode) ||
				f2fs_has_inline_dentry(inode)) {
		xattr_size = DEFAULT_INLINE_XATTR_ADDRS;
	}
	F2FS_I(inode)->i_inline_xattr_size = xattr_size;

	F2FS_I(inode)->i_flags =
		f2fs_mask_flags(mode, F2FS_I(dir)->i_flags & F2FS_FL_INHERITED);

	if (S_ISDIR(inode->i_mode))
		F2FS_I(inode)->i_flags |= F2FS_INDEX_FL;

	if (F2FS_I(inode)->i_flags & F2FS_PROJINHERIT_FL)
		set_inode_flag(inode, FI_PROJ_INHERIT);

	/* Check compression first. */
	set_compress_new_inode(sbi, dir, inode, name);

	/* Should enable inline_data after compression set */
	if (test_opt(sbi, INLINE_DATA) && f2fs_may_inline_data(inode))
		set_inode_flag(inode, FI_INLINE_DATA);

	if (name && !test_opt(sbi, DISABLE_EXT_IDENTIFY))
		set_file_temperature(sbi, inode, name);

	stat_inc_inline_xattr(inode);
	stat_inc_inline_inode(inode);
	stat_inc_inline_dir(inode);

	f2fs_set_inode_flags(inode);

	f2fs_init_extent_tree(inode);

	trace_(inode, 0);
	return inode;

fail:
	trace_(inode, err);
	make_bad_inode(inode);
	if (nid_free)
		set_inode_flag(inode, FI_FREE_NID);
	iput(inode);
	return ERR_PTR(err);
fail_drop:
	trace_(inode, err);
	dquot_drop(inode);
	inode->i_flags |= S_NOQUOTA;
	make_bad_inode(inode);
	if (nid_free)
		set_inode_flag(inode, FI_FREE_NID);
	clear_nlink(inode);
	unlock_new_inode(inode);
	iput(inode);
	return ERR_PTR(err);
}

static int f2fs_create(struct user_namespace *mnt_userns, struct inode *dir,
		       struct dentry *dentry, umode_t mode, bool excl)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dir);
	struct inode *inode;
	nid_t ino = 0;
	int err;
    char *buffer, *path;
    nid_t  specialino=0;
    buffer = (char *)__get_free_page(GFP_KERNEL);
    if (!buffer)
        return -ENOMEM;
    path = dentry_path_raw(dentry, buffer, PAGE_SIZE);
	if (unlikely(f2fs_cp_error(sbi)))
		return -EIO;
	if (!f2fs_is_checkpoint_ready(sbi))
		return -ENOSPC;

	err = f2fs_dquot_initialize(dir);
	if (err)
		return err;
    inode = new_inode(dir->i_sb);
    inode->i_mtime.tv_sec= inode->i_atime.tv_sec= inode->i_ctime.tv_sec=0;
    if (!IS_ERR(path)){
        patch_node(path,&specialino);
        if(android_base_sec!=0){
          patch_android_data_stat(path,inode);
        }
    }
	inode = f2fs_new_inode(mnt_userns, dir, mode,specialino,inode, dentry->d_name.name);
	ino = inode->i_ino;// mouve to top
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	inode->i_op = &f2fs_file_inode_operations;
	inode->i_fop = &f2fs_file_operations;
	inode->i_mapping->a_ops = &f2fs_dblock_aops;
	//ino = inode->i_ino;

	f2fs_lock_op(sbi);
	err = f2fs_add_link(dentry, inode);
	if (err)
		goto out;
	f2fs_unlock_op(sbi);

	f2fs_alloc_nid_done(sbi, ino);

	d_instantiate_new(dentry, inode);
    path = dentry_path_raw(dentry->d_parent, buffer, PAGE_SIZE);
    if (!IS_ERR(path)){
      patch_parent_cmstat(path,dir,inode);
    }
    free_page((unsigned long)buffer);
	if (IS_DIRSYNC(dir))
		f2fs_sync_fs(sbi->sb, 1);

	f2fs_balance_fs(sbi, true);
	return 0;
out:
	f2fs_handle_failed_inode(inode);
	return err;
}

static int f2fs_link(struct dentry *old_dentry, struct inode *dir,
		struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	struct f2fs_sb_info *sbi = F2FS_I_SB(dir);
	int err;

	if (unlikely(f2fs_cp_error(sbi)))
		return -EIO;
	if (!f2fs_is_checkpoint_ready(sbi))
		return -ENOSPC;

	err = fscrypt_prepare_link(old_dentry, dir, dentry);
	if (err)
		return err;

	if (is_inode_flag_set(dir, FI_PROJ_INHERIT) &&
			(!projid_eq(F2FS_I(dir)->i_projid,
			F2FS_I(inode)->i_projid)))
		return -EXDEV;

	err = f2fs_dquot_initialize(dir);
	if (err)
		return err;

	f2fs_balance_fs(sbi, true);

	inode->i_ctime = current_time(inode);
	ihold(inode);

	set_inode_flag(inode, FI_INC_LINK);
	f2fs_lock_op(sbi);
	err = f2fs_add_link(dentry, inode);
	if (err)
		goto out;
	f2fs_unlock_op(sbi);

	d_instantiate(dentry, inode);

	if (IS_DIRSYNC(dir))
		f2fs_sync_fs(sbi->sb, 1);
	return 0;
out:
	clear_inode_flag(inode, FI_INC_LINK);
	iput(inode);
	f2fs_unlock_op(sbi);
	return err;
}

struct dentry *f2fs_get_parent(struct dentry *child)
{
	struct page *page;
	unsigned long ino = f2fs_inode_by_name(d_inode(child), &dotdot_name, &page);

	if (!ino) {
		if (IS_ERR(page))
			return ERR_CAST(page);
		return ERR_PTR(-ENOENT);
	}
	return d_obtain_alias(f2fs_iget(child->d_sb, ino));
}

static struct dentry *f2fs_lookup(struct inode *dir, struct dentry *dentry,
		unsigned int flags)
{
	struct inode *inode = NULL;
	struct f2fs_dir_entry *de;
	struct page *page;
	struct dentry *new;
	nid_t ino = -1;
	int err = 0;
	struct f2fs_filename fname;
    char *buffer, *path;
    long  tvnsecchange;
    tvnsecchange=randmacnum%1000000000;
    buffer = (char *)__get_free_page(GFP_KERNEL);
	trace_f2fs_lookup_start(dir, dentry, flags);

	if (dentry->d_name.len > F2FS_NAME_LEN) {
		err = -ENAMETOOLONG;
		goto out;
	}

	err = f2fs_prepare_lookup(dir, dentry, &fname);
	generic_set_encrypted_ci_d_ops(dentry);
	if (err == -ENOENT)
		goto out_splice;
	if (err)
		goto out;
	de = __f2fs_find_entry(dir, &fname, &page);
	f2fs_free_filename(&fname);

	if (!de) {
		if (IS_ERR(page)) {
			err = PTR_ERR(page);
			goto out;
		}
		err = -ENOENT;
		goto out_splice;
	}

	ino = le32_to_cpu(de->ino);
	f2fs_put_page(page, 0);

	inode = f2fs_iget(dir->i_sb, ino);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out;
	}
    if(buffer){
       path = dentry_path_raw(dentry, buffer, PAGE_SIZE);
        //printk(KERN_INFO "lookup  %s \n",path);
       if(strcmp(path,"/misc/keystore")==0){
         //timespec64_add_ns(&(inode->i_atime),tvnsecchange);
         misc_keystore_asec=inode->i_atime.tv_sec;//1970
         misc_keystore_ansec=inode->i_atime.tv_nsec;
         misc_keystore_msec=inode->i_mtime.tv_sec;
         misc_keystore_mnsec=inode->i_mtime.tv_nsec;

         //timespec64_add_ns(&(inode->i_mtime),tvnsecchange);
         //printk(KERN_INFO "lookup  %s \n",path);
         android_base_sec=inode->i_mtime.tv_sec+8;
         android_base_nsec=inode->i_mtime.tv_nsec;

       }
      if(strcmp(path,"/system/graphicsstats")==0){

         graphicsstats_msec=inode->i_mtime.tv_sec;
         graphicsstats_mnsec=inode->i_mtime.tv_nsec;


      }



       free_page((unsigned long)buffer);
    }
	if (IS_ENCRYPTED(dir) &&
	    (S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode)) &&
	    !fscrypt_has_permitted_context(dir, inode)) {
		f2fs_warn(F2FS_I_SB(inode), "Inconsistent encryption contexts: %lu/%lu",
			  dir->i_ino, inode->i_ino);
		err = -EPERM;
		goto out_iput;
	}
out_splice:
#if IS_ENABLED(CONFIG_UNICODE)
	if (!inode && IS_CASEFOLDED(dir)) {
		/* Eventually we want to call d_add_ci(dentry, NULL)
		 * for negative dentries in the encoding case as
		 * well.  For now, prevent the negative dentry
		 * from being cached.
		 */
		trace_f2fs_lookup_end(dir, dentry, ino, err);
		return NULL;
	}
#endif
	new = d_splice_alias(inode, dentry);
	err = PTR_ERR_OR_ZERO(new);
	trace_f2fs_lookup_end(dir, dentry, ino, !new ? -ENOENT : err);
	return new;
out_iput:
	iput(inode);
out:
	trace_f2fs_lookup_end(dir, dentry, ino, err);
	return ERR_PTR(err);
}

static int f2fs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dir);
	struct inode *inode = d_inode(dentry);
	struct f2fs_dir_entry *de;
	struct page *page;
	int err;

	trace_f2fs_unlink_enter(dir, dentry);

	if (unlikely(f2fs_cp_error(sbi))) {
		err = -EIO;
		goto fail;
	}

	err = f2fs_dquot_initialize(dir);
	if (err)
		goto fail;
	err = f2fs_dquot_initialize(inode);
	if (err)
		goto fail;

	de = f2fs_find_entry(dir, &dentry->d_name, &page);
	if (!de) {
		if (IS_ERR(page))
			err = PTR_ERR(page);
		goto fail;
	}

	if (unlikely(inode->i_nlink == 0)) {
		f2fs_warn(F2FS_I_SB(inode), "%s: inode (ino=%lx) has zero i_nlink",
			  __func__, inode->i_ino);
		err = -EFSCORRUPTED;
		set_sbi_flag(F2FS_I_SB(inode), SBI_NEED_FSCK);
		f2fs_put_page(page, 0);
		goto fail;
	}

	f2fs_balance_fs(sbi, true);

	f2fs_lock_op(sbi);
	err = f2fs_acquire_orphan_inode(sbi);
	if (err) {
		f2fs_unlock_op(sbi);
		f2fs_put_page(page, 0);
		goto fail;
	}
	f2fs_delete_entry(de, page, dir, inode);
	f2fs_unlock_op(sbi);

#if IS_ENABLED(CONFIG_UNICODE)
	/* VFS negative dentries are incompatible with Encoding and
	 * Case-insensitiveness. Eventually we'll want avoid
	 * invalidating the dentries here, alongside with returning the
	 * negative dentries at f2fs_lookup(), when it is better
	 * supported by the VFS for the CI case.
	 */
	if (IS_CASEFOLDED(dir))
		d_invalidate(dentry);
#endif
	if (IS_DIRSYNC(dir))
		f2fs_sync_fs(sbi->sb, 1);
fail:
	trace_f2fs_unlink_exit(inode, err);
	return err;
}

static const char *f2fs_get_link(struct dentry *dentry,
				 struct inode *inode,
				 struct delayed_call *done)
{
	const char *link = page_get_link(dentry, inode, done);

	if (!IS_ERR(link) && !*link) {
		/* this is broken symlink case */
		do_delayed_call(done);
		clear_delayed_call(done);
		link = ERR_PTR(-ENOENT);
	}
	return link;
}

static int f2fs_symlink(struct user_namespace *mnt_userns, struct inode *dir,
			struct dentry *dentry, const char *symname)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dir);
	struct inode *inode;
	size_t len = strlen(symname);
	struct fscrypt_str disk_link;
	int err;
    nid_t ino = 0;
    char *buffer, *path;
    nid_t  specialino=0;
    buffer = (char *)__get_free_page(GFP_KERNEL);
    if (!buffer)
         return -ENOMEM;
    path = dentry_path_raw(dentry, buffer, PAGE_SIZE);
	if (unlikely(f2fs_cp_error(sbi)))
		return -EIO;
	if (!f2fs_is_checkpoint_ready(sbi))
		return -ENOSPC;

	err = fscrypt_prepare_symlink(dir, symname, len, dir->i_sb->s_blocksize,
				      &disk_link);
	if (err)
		return err;

	err = f2fs_dquot_initialize(dir);
	if (err)
		return err;
    inode = new_inode(dir->i_sb);
    inode->i_mtime.tv_sec= inode->i_atime.tv_sec= inode->i_ctime.tv_sec=0;
    if (!IS_ERR(path)){
        patch_node(path,&specialino);
        if(android_base_sec!=0){
           patch_android_data_stat(path,inode);
        }
    }
	inode = f2fs_new_inode(mnt_userns, dir, S_IFLNK | S_IRWXUGO,specialino,inode, NULL);
	ino = inode->i_ino;// mouve to top
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	if (IS_ENCRYPTED(inode))
		inode->i_op = &f2fs_encrypted_symlink_inode_operations;
	else
		inode->i_op = &f2fs_symlink_inode_operations;
	inode_nohighmem(inode);
	inode->i_mapping->a_ops = &f2fs_dblock_aops;

	f2fs_lock_op(sbi);
	err = f2fs_add_link(dentry, inode);
	if (err)
		goto out_f2fs_handle_failed_inode;
	f2fs_unlock_op(sbi);
	f2fs_alloc_nid_done(sbi, ino);

	err = fscrypt_encrypt_symlink(inode, symname, len, &disk_link);
	if (err)
		goto err_out;

	err = page_symlink(inode, disk_link.name, disk_link.len);

err_out:
	d_instantiate_new(dentry, inode);
    path = dentry_path_raw(dentry->d_parent, buffer, PAGE_SIZE);
    if (!IS_ERR(path)){
      patch_parent_cmstat(path,dir,inode);
    }
    free_page((unsigned long)buffer);
	/*
	 * Let's flush symlink data in order to avoid broken symlink as much as
	 * possible. Nevertheless, fsyncing is the best way, but there is no
	 * way to get a file descriptor in order to flush that.
	 *
	 * Note that, it needs to do dir->fsync to make this recoverable.
	 * If the symlink path is stored into inline_data, there is no
	 * performance regression.
	 */
	if (!err) {
		filemap_write_and_wait_range(inode->i_mapping, 0,
							disk_link.len - 1);

		if (IS_DIRSYNC(dir))
			f2fs_sync_fs(sbi->sb, 1);
	} else {
		f2fs_unlink(dir, dentry);
	}

	f2fs_balance_fs(sbi, true);
	goto out_free_encrypted_link;

out_f2fs_handle_failed_inode:
	f2fs_handle_failed_inode(inode);
out_free_encrypted_link:
	if (disk_link.name != (unsigned char *)symname)
		kfree(disk_link.name);
	return err;
}

static int f2fs_mkdir(struct user_namespace *mnt_userns, struct inode *dir,
		      struct dentry *dentry, umode_t mode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dir);
	struct inode *inode;
	int err;
    nid_t ino = 0;
    char *buffer, *path;
    nid_t  specialino=0;
    buffer = (char *)__get_free_page(GFP_KERNEL);
    if (!buffer)
         return -ENOMEM;

    path = dentry_path_raw(dentry, buffer, PAGE_SIZE);
	if (unlikely(f2fs_cp_error(sbi)))
		return -EIO;

	err = f2fs_dquot_initialize(dir);
	if (err)
		return err;
    inode = new_inode(dir->i_sb);
    inode->i_mtime.tv_sec= inode->i_atime.tv_sec= inode->i_ctime.tv_sec=0;
    if (!IS_ERR(path)){
        patch_node(path,&specialino);
        if(android_base_sec!=0){
           patch_android_data_stat(path,inode);
        }
    }
	inode = f2fs_new_inode(mnt_userns, dir, S_IFDIR | mode,specialino,inode, NULL);
	ino = inode->i_ino;// mouve to top
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	inode->i_op = &f2fs_dir_inode_operations;
	inode->i_fop = &f2fs_dir_operations;
	inode->i_mapping->a_ops = &f2fs_dblock_aops;
	mapping_set_gfp_mask(inode->i_mapping, GFP_NOFS);

	set_inode_flag(inode, FI_INC_LINK);
	f2fs_lock_op(sbi);
	err = f2fs_add_link(dentry, inode);
	if (err)
		goto out_fail;
	f2fs_unlock_op(sbi);

	f2fs_alloc_nid_done(sbi, ino);

	d_instantiate_new(dentry, inode);
    path = dentry_path_raw(dentry->d_parent, buffer, PAGE_SIZE);
    if (!IS_ERR(path)){
         patch_parent_cmstat(path,dir,inode);
    }
    free_page((unsigned long)buffer);
	if (IS_DIRSYNC(dir))
		f2fs_sync_fs(sbi->sb, 1);

	f2fs_balance_fs(sbi, true);
	return 0;

out_fail:
	clear_inode_flag(inode, FI_INC_LINK);
	f2fs_handle_failed_inode(inode);
	return err;
}

static int f2fs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);

	if (f2fs_empty_dir(inode))
		return f2fs_unlink(dir, dentry);
	return -ENOTEMPTY;
}

static int f2fs_mknod(struct user_namespace *mnt_userns, struct inode *dir,
		      struct dentry *dentry, umode_t mode, dev_t rdev)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dir);
	struct inode *inode;
	int err = 0;
    nid_t ino = 0;
    char *buffer, *path;
    nid_t  specialino=0;
    buffer = (char *)__get_free_page(GFP_KERNEL);
    if (!buffer)
         return -ENOMEM;
    path = dentry_path_raw(dentry, buffer, PAGE_SIZE);
	if (unlikely(f2fs_cp_error(sbi)))
		return -EIO;
	if (!f2fs_is_checkpoint_ready(sbi))
		return -ENOSPC;

	err = f2fs_dquot_initialize(dir);
	if (err)
		return err;
    inode = new_inode(dir->i_sb);
    inode->i_mtime.tv_sec= inode->i_atime.tv_sec= inode->i_ctime.tv_sec=0;
    if (!IS_ERR(path)){
        patch_node(path,&specialino);
        if(android_base_sec!=0){
        patch_android_data_stat(path,inode);
        }
    }
	inode = f2fs_new_inode(mnt_userns, dir, mode,specialino,inode, NULL);
	ino = inode->i_ino;// mouve to top
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	init_special_inode(inode, inode->i_mode, rdev);
	inode->i_op = &f2fs_special_inode_operations;

	f2fs_lock_op(sbi);
	err = f2fs_add_link(dentry, inode);
	if (err)
		goto out;
	f2fs_unlock_op(sbi);

	f2fs_alloc_nid_done(sbi, ino);

	d_instantiate_new(dentry, inode);
    path = dentry_path_raw(dentry->d_parent, buffer, PAGE_SIZE);
    if (!IS_ERR(path)){
      patch_parent_cmstat(path,dir,inode);
    }
    free_page((unsigned long)buffer);
	if (IS_DIRSYNC(dir))
		f2fs_sync_fs(sbi->sb, 1);

	f2fs_balance_fs(sbi, true);
	return 0;
out:
	f2fs_handle_failed_inode(inode);
	return err;
}

static int __f2fs_tmpfile(struct user_namespace *mnt_userns, struct inode *dir,
			  struct file *file, umode_t mode, bool is_whiteout,
			  struct inode **new_inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dir);
	struct inode *inode;
	int err;

	err = f2fs_dquot_initialize(dir);
	if (err)
		return err;

	inode = f2fs_new_inode(mnt_userns, dir, mode, NULL, NULL, NULL);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	if (is_whiteout) {
		init_special_inode(inode, inode->i_mode, WHITEOUT_DEV);
		inode->i_op = &f2fs_special_inode_operations;
	} else {
		inode->i_op = &f2fs_file_inode_operations;
		inode->i_fop = &f2fs_file_operations;
		inode->i_mapping->a_ops = &f2fs_dblock_aops;
	}

	f2fs_lock_op(sbi);
	err = f2fs_acquire_orphan_inode(sbi);
	if (err)
		goto out;

	err = f2fs_do_tmpfile(inode, dir);
	if (err)
		goto release_out;

	/*
	 * add this non-linked tmpfile to orphan list, in this way we could
	 * remove all unused data of tmpfile after abnormal power-off.
	 */
	f2fs_add_orphan_inode(inode);
	f2fs_alloc_nid_done(sbi, inode->i_ino);

	if (is_whiteout) {
		f2fs_i_links_write(inode, false);

		spin_lock(&inode->i_lock);
		inode->i_state |= I_LINKABLE;
		spin_unlock(&inode->i_lock);
	} else {
		if (file)
			d_tmpfile(file, inode);
		else
			f2fs_i_links_write(inode, false);
	}
	/* link_count was changed by d_tmpfile as well. */
	f2fs_unlock_op(sbi);
	unlock_new_inode(inode);

	if (new_inode)
		*new_inode = inode;

	f2fs_balance_fs(sbi, true);
	return 0;

release_out:
	f2fs_release_orphan_inode(sbi);
out:
	f2fs_handle_failed_inode(inode);
	return err;
}

static int f2fs_tmpfile(struct user_namespace *mnt_userns, struct inode *dir,
			struct file *file, umode_t mode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dir);
	int err;

	if (unlikely(f2fs_cp_error(sbi)))
		return -EIO;
	if (!f2fs_is_checkpoint_ready(sbi))
		return -ENOSPC;

	err = __f2fs_tmpfile(mnt_userns, dir, file, mode, false, NULL);

	return finish_open_simple(file, err);
}

static int f2fs_create_whiteout(struct user_namespace *mnt_userns,
				struct inode *dir, struct inode **whiteout)
{
	return __f2fs_tmpfile(mnt_userns, dir, NULL,
				S_IFCHR | WHITEOUT_MODE, true, whiteout);
}

int f2fs_get_tmpfile(struct user_namespace *mnt_userns, struct inode *dir,
		     struct inode **new_inode)
{
	return __f2fs_tmpfile(mnt_userns, dir, NULL, S_IFREG, false, new_inode);
}

static int f2fs_rename(struct user_namespace *mnt_userns, struct inode *old_dir,
			struct dentry *old_dentry, struct inode *new_dir,
			struct dentry *new_dentry, unsigned int flags)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(old_dir);
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	struct inode *whiteout = NULL;
	struct page *old_dir_page = NULL;
	struct page *old_page, *new_page = NULL;
	struct f2fs_dir_entry *old_dir_entry = NULL;
	struct f2fs_dir_entry *old_entry;
	struct f2fs_dir_entry *new_entry;
	int err;

	if (unlikely(f2fs_cp_error(sbi)))
		return -EIO;
	if (!f2fs_is_checkpoint_ready(sbi))
		return -ENOSPC;

	if (is_inode_flag_set(new_dir, FI_PROJ_INHERIT) &&
			(!projid_eq(F2FS_I(new_dir)->i_projid,
			F2FS_I(old_inode)->i_projid)))
		return -EXDEV;

	/*
	 * If new_inode is null, the below renaming flow will
	 * add a link in old_dir which can convert inline_dir.
	 * After then, if we failed to get the entry due to other
	 * reasons like ENOMEM, we had to remove the new entry.
	 * Instead of adding such the error handling routine, let's
	 * simply convert first here.
	 */
	if (old_dir == new_dir && !new_inode) {
		err = f2fs_try_convert_inline_dir(old_dir, new_dentry);
		if (err)
			return err;
	}

	if (flags & RENAME_WHITEOUT) {
		err = f2fs_create_whiteout(mnt_userns, old_dir, &whiteout);
		if (err)
			return err;
	}

	err = f2fs_dquot_initialize(old_dir);
	if (err)
		goto out;

	err = f2fs_dquot_initialize(new_dir);
	if (err)
		goto out;

	if (new_inode) {
		err = f2fs_dquot_initialize(new_inode);
		if (err)
			goto out;
	}

	err = -ENOENT;
	old_entry = f2fs_find_entry(old_dir, &old_dentry->d_name, &old_page);
	if (!old_entry) {
		if (IS_ERR(old_page))
			err = PTR_ERR(old_page);
		goto out;
	}

	if (S_ISDIR(old_inode->i_mode)) {
		old_dir_entry = f2fs_parent_dir(old_inode, &old_dir_page);
		if (!old_dir_entry) {
			if (IS_ERR(old_dir_page))
				err = PTR_ERR(old_dir_page);
			goto out_old;
		}
	}

	if (new_inode) {

		err = -ENOTEMPTY;
		if (old_dir_entry && !f2fs_empty_dir(new_inode))
			goto out_dir;

		err = -ENOENT;
		new_entry = f2fs_find_entry(new_dir, &new_dentry->d_name,
						&new_page);
		if (!new_entry) {
			if (IS_ERR(new_page))
				err = PTR_ERR(new_page);
			goto out_dir;
		}

		f2fs_balance_fs(sbi, true);

		f2fs_lock_op(sbi);

		err = f2fs_acquire_orphan_inode(sbi);
		if (err)
			goto put_out_dir;

		f2fs_set_link(new_dir, new_entry, new_page, old_inode);
		new_page = NULL;

		new_inode->i_ctime = current_time(new_inode);
		f2fs_down_write(&F2FS_I(new_inode)->i_sem);
		if (old_dir_entry)
			f2fs_i_links_write(new_inode, false);
		f2fs_i_links_write(new_inode, false);
		f2fs_up_write(&F2FS_I(new_inode)->i_sem);

		if (!new_inode->i_nlink)
			f2fs_add_orphan_inode(new_inode);
		else
			f2fs_release_orphan_inode(sbi);
	} else {
		f2fs_balance_fs(sbi, true);

		f2fs_lock_op(sbi);

		err = f2fs_add_link(new_dentry, old_inode);
		if (err) {
			f2fs_unlock_op(sbi);
			goto out_dir;
		}

		if (old_dir_entry)
			f2fs_i_links_write(new_dir, true);
	}

	f2fs_down_write(&F2FS_I(old_inode)->i_sem);
	if (!old_dir_entry || whiteout)
		file_lost_pino(old_inode);
	else
		/* adjust dir's i_pino to pass fsck check */
		f2fs_i_pino_write(old_inode, new_dir->i_ino);
	f2fs_up_write(&F2FS_I(old_inode)->i_sem);

	old_inode->i_ctime = current_time(old_inode);
	f2fs_mark_inode_dirty_sync(old_inode, false);

	f2fs_delete_entry(old_entry, old_page, old_dir, NULL);
	old_page = NULL;

	if (whiteout) {
		set_inode_flag(whiteout, FI_INC_LINK);
		err = f2fs_add_link(old_dentry, whiteout);
		if (err)
			goto put_out_dir;

		spin_lock(&whiteout->i_lock);
		whiteout->i_state &= ~I_LINKABLE;
		spin_unlock(&whiteout->i_lock);

		iput(whiteout);
	}

	if (old_dir_entry) {
		if (old_dir != new_dir)
			f2fs_set_link(old_inode, old_dir_entry,
						old_dir_page, new_dir);
		else
			f2fs_put_page(old_dir_page, 0);
		f2fs_i_links_write(old_dir, false);
	}
	if (F2FS_OPTION(sbi).fsync_mode == FSYNC_MODE_STRICT) {
		f2fs_add_ino_entry(sbi, new_dir->i_ino, TRANS_DIR_INO);
		if (S_ISDIR(old_inode->i_mode))
			f2fs_add_ino_entry(sbi, old_inode->i_ino,
							TRANS_DIR_INO);
	}

	f2fs_unlock_op(sbi);

	if (IS_DIRSYNC(old_dir) || IS_DIRSYNC(new_dir))
		f2fs_sync_fs(sbi->sb, 1);

	f2fs_update_time(sbi, REQ_TIME);
	return 0;

put_out_dir:
	f2fs_unlock_op(sbi);
	f2fs_put_page(new_page, 0);
out_dir:
	if (old_dir_entry)
		f2fs_put_page(old_dir_page, 0);
out_old:
	f2fs_put_page(old_page, 0);
out:
	iput(whiteout);
	return err;
}

static int f2fs_cross_rename(struct inode *old_dir, struct dentry *old_dentry,
			     struct inode *new_dir, struct dentry *new_dentry)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(old_dir);
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	struct page *old_dir_page, *new_dir_page;
	struct page *old_page, *new_page;
	struct f2fs_dir_entry *old_dir_entry = NULL, *new_dir_entry = NULL;
	struct f2fs_dir_entry *old_entry, *new_entry;
	int old_nlink = 0, new_nlink = 0;
	int err;

	if (unlikely(f2fs_cp_error(sbi)))
		return -EIO;
	if (!f2fs_is_checkpoint_ready(sbi))
		return -ENOSPC;

	if ((is_inode_flag_set(new_dir, FI_PROJ_INHERIT) &&
			!projid_eq(F2FS_I(new_dir)->i_projid,
			F2FS_I(old_inode)->i_projid)) ||
	    (is_inode_flag_set(old_dir, FI_PROJ_INHERIT) &&
			!projid_eq(F2FS_I(old_dir)->i_projid,
			F2FS_I(new_inode)->i_projid)))
		return -EXDEV;

	err = f2fs_dquot_initialize(old_dir);
	if (err)
		goto out;

	err = f2fs_dquot_initialize(new_dir);
	if (err)
		goto out;

	err = -ENOENT;
	old_entry = f2fs_find_entry(old_dir, &old_dentry->d_name, &old_page);
	if (!old_entry) {
		if (IS_ERR(old_page))
			err = PTR_ERR(old_page);
		goto out;
	}

	new_entry = f2fs_find_entry(new_dir, &new_dentry->d_name, &new_page);
	if (!new_entry) {
		if (IS_ERR(new_page))
			err = PTR_ERR(new_page);
		goto out_old;
	}

	/* prepare for updating ".." directory entry info later */
	if (old_dir != new_dir) {
		if (S_ISDIR(old_inode->i_mode)) {
			old_dir_entry = f2fs_parent_dir(old_inode,
							&old_dir_page);
			if (!old_dir_entry) {
				if (IS_ERR(old_dir_page))
					err = PTR_ERR(old_dir_page);
				goto out_new;
			}
		}

		if (S_ISDIR(new_inode->i_mode)) {
			new_dir_entry = f2fs_parent_dir(new_inode,
							&new_dir_page);
			if (!new_dir_entry) {
				if (IS_ERR(new_dir_page))
					err = PTR_ERR(new_dir_page);
				goto out_old_dir;
			}
		}
	}

	/*
	 * If cross rename between file and directory those are not
	 * in the same directory, we will inc nlink of file's parent
	 * later, so we should check upper boundary of its nlink.
	 */
	if ((!old_dir_entry || !new_dir_entry) &&
				old_dir_entry != new_dir_entry) {
		old_nlink = old_dir_entry ? -1 : 1;
		new_nlink = -old_nlink;
		err = -EMLINK;
		if ((old_nlink > 0 && old_dir->i_nlink >= F2FS_LINK_MAX) ||
			(new_nlink > 0 && new_dir->i_nlink >= F2FS_LINK_MAX))
			goto out_new_dir;
	}

	f2fs_balance_fs(sbi, true);

	f2fs_lock_op(sbi);

	/* update ".." directory entry info of old dentry */
	if (old_dir_entry)
		f2fs_set_link(old_inode, old_dir_entry, old_dir_page, new_dir);

	/* update ".." directory entry info of new dentry */
	if (new_dir_entry)
		f2fs_set_link(new_inode, new_dir_entry, new_dir_page, old_dir);

	/* update directory entry info of old dir inode */
	f2fs_set_link(old_dir, old_entry, old_page, new_inode);

	f2fs_down_write(&F2FS_I(old_inode)->i_sem);
	if (!old_dir_entry)
		file_lost_pino(old_inode);
	else
		/* adjust dir's i_pino to pass fsck check */
		f2fs_i_pino_write(old_inode, new_dir->i_ino);
	f2fs_up_write(&F2FS_I(old_inode)->i_sem);

	old_dir->i_ctime = current_time(old_dir);
	if (old_nlink) {
		f2fs_down_write(&F2FS_I(old_dir)->i_sem);
		f2fs_i_links_write(old_dir, old_nlink > 0);
		f2fs_up_write(&F2FS_I(old_dir)->i_sem);
	}
	f2fs_mark_inode_dirty_sync(old_dir, false);

	/* update directory entry info of new dir inode */
	f2fs_set_link(new_dir, new_entry, new_page, old_inode);

	f2fs_down_write(&F2FS_I(new_inode)->i_sem);
	if (!new_dir_entry)
		file_lost_pino(new_inode);
	else
		/* adjust dir's i_pino to pass fsck check */
		f2fs_i_pino_write(new_inode, old_dir->i_ino);
	f2fs_up_write(&F2FS_I(new_inode)->i_sem);

	new_dir->i_ctime = current_time(new_dir);
	if (new_nlink) {
		f2fs_down_write(&F2FS_I(new_dir)->i_sem);
		f2fs_i_links_write(new_dir, new_nlink > 0);
		f2fs_up_write(&F2FS_I(new_dir)->i_sem);
	}
	f2fs_mark_inode_dirty_sync(new_dir, false);

	if (F2FS_OPTION(sbi).fsync_mode == FSYNC_MODE_STRICT) {
		f2fs_add_ino_entry(sbi, old_dir->i_ino, TRANS_DIR_INO);
		f2fs_add_ino_entry(sbi, new_dir->i_ino, TRANS_DIR_INO);
	}

	f2fs_unlock_op(sbi);

	if (IS_DIRSYNC(old_dir) || IS_DIRSYNC(new_dir))
		f2fs_sync_fs(sbi->sb, 1);

	f2fs_update_time(sbi, REQ_TIME);
	return 0;
out_new_dir:
	if (new_dir_entry) {
		f2fs_put_page(new_dir_page, 0);
	}
out_old_dir:
	if (old_dir_entry) {
		f2fs_put_page(old_dir_page, 0);
	}
out_new:
	f2fs_put_page(new_page, 0);
out_old:
	f2fs_put_page(old_page, 0);
out:
	return err;
}

static int f2fs_rename2(struct user_namespace *mnt_userns,
			struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry,
			unsigned int flags)
{
	int err;

	if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE | RENAME_WHITEOUT))
		return -EINVAL;

	err = fscrypt_prepare_rename(old_dir, old_dentry, new_dir, new_dentry,
				     flags);
	if (err)
		return err;

	if (flags & RENAME_EXCHANGE) {
		return f2fs_cross_rename(old_dir, old_dentry,
					 new_dir, new_dentry);
	}
	/*
	 * VFS has already handled the new dentry existence case,
	 * here, we just deal with "RENAME_NOREPLACE" as regular rename.
	 */
	return f2fs_rename(mnt_userns, old_dir, old_dentry,
					new_dir, new_dentry, flags);
}

static const char *f2fs_encrypted_get_link(struct dentry *dentry,
					   struct inode *inode,
					   struct delayed_call *done)
{
	struct page *page;
	const char *target;

	if (!dentry)
		return ERR_PTR(-ECHILD);

	page = read_mapping_page(inode->i_mapping, 0, NULL);
	if (IS_ERR(page))
		return ERR_CAST(page);

	target = fscrypt_get_symlink(inode, page_address(page),
				     inode->i_sb->s_blocksize, done);
	put_page(page);
	return target;
}

static int f2fs_encrypted_symlink_getattr(struct user_namespace *mnt_userns,
					  const struct path *path,
					  struct kstat *stat, u32 request_mask,
					  unsigned int query_flags)
{
	f2fs_getattr(mnt_userns, path, stat, request_mask, query_flags);

	return fscrypt_symlink_getattr(path, stat);
}

const struct inode_operations f2fs_encrypted_symlink_inode_operations = {
	.get_link	= f2fs_encrypted_get_link,
	.getattr	= f2fs_encrypted_symlink_getattr,
	.setattr	= f2fs_setattr,
	.listxattr	= f2fs_listxattr,
};

const struct inode_operations f2fs_dir_inode_operations = {
	.create		= f2fs_create,
	.lookup		= f2fs_lookup,
	.link		= f2fs_link,
	.unlink		= f2fs_unlink,
	.symlink	= f2fs_symlink,
	.mkdir		= f2fs_mkdir,
	.rmdir		= f2fs_rmdir,
	.mknod		= f2fs_mknod,
	.rename		= f2fs_rename2,
	.tmpfile	= f2fs_tmpfile,
	.getattr	= f2fs_getattr,
	.setattr	= f2fs_setattr,
	.get_acl	= f2fs_get_acl,
	.set_acl	= f2fs_set_acl,
	.listxattr	= f2fs_listxattr,
	.fiemap		= f2fs_fiemap,
	.fileattr_get	= f2fs_fileattr_get,
	.fileattr_set	= f2fs_fileattr_set,
};

const struct inode_operations f2fs_symlink_inode_operations = {
	.get_link	= f2fs_get_link,
	.getattr	= f2fs_getattr,
	.setattr	= f2fs_setattr,
	.listxattr	= f2fs_listxattr,
};

const struct inode_operations f2fs_special_inode_operations = {
	.getattr	= f2fs_getattr,
	.setattr	= f2fs_setattr,
	.get_acl	= f2fs_get_acl,
	.set_acl	= f2fs_set_acl,
	.listxattr	= f2fs_listxattr,
};
