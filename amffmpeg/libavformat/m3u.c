/*
 *m3u for ffmpeg system
 * Copyright (c) 2001 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include "libavutil/avstring.h"
#include "avformat.h"
#include <fcntl.h>
#if HAVE_SETMODE
#include <io.h>
#endif
#include <unistd.h>
#include <sys/time.h>
#include <stdlib.h>
#include "os_support.h"
#include "file_list.h"
#include "internal.h"
#include "libavutil/intreadwrite.h"

#define EXTM3U						"#EXTM3U"
#define EXTINF						"#EXTINF"
#define EXT_X_TARGETDURATION		"#EXT-X-TARGETDURATION"

#define EXT_X_MEDIA_SEQUENCE		"#EXT-X-MEDIA-SEQUENCE"
#define EXT_X_KEY					"#EXT-X-KEY"
#define EXT_X_PROGRAM_DATE_TIME	"#EXT-X-PROGRAM-DATE-TIME"
#define EXT_X_ALLOW_CACHE			"#EXT-X-ALLOW-CACHE"
#define EXT_X_ENDLIST				"#EXT-X-ENDLIST"
#define EXT_X_STREAM_INF			"#EXT-X-STREAM-INF"

#define EXT_X_DISCONTINUITY		"#EXT-X-DISCONTINUITY"

#define is_TAG(l,tag)	(!strncmp(l,tag,strlen(tag)))
#define is_NET_URL(url)		(!strncmp(url,"http://",7) || !strncmp(url,"shttp://",8)||!strncmp(url,"shttps://",9))

struct m3u_info{
	int duration;
	int sequence;
	int allow_cache;
	int endlist;
	char *key;
	char *data_time;
	char *streaminfo;
	char *file;
};



static int m3u_format_get_line(ByteIOContext *s,char *line,int line_size)
{
    int ch;
    char *q;
	if(s->eof_reached || s->error)
		return -1;
    q = line;
    for(;;) {
        ch = get_byte(s);
        if (ch < 0)
            return AVERROR(EIO);
        if (ch == '\n' || ch == '\0') {
            /* process line */
            if (q > line && q[-1] == '\r')
                q--;
            *q = '\0';
	     av_log(NULL, AV_LOG_INFO, "m3u_format_get_line line %d=%s\n",sizeof(line),line);
            return q-line;
        } else {
            if ((q - line) < line_size - 1)
                *q++ = ch;
        }
    }
	return 0;
}

struct variant_info {
    char bandwidth[20];
};

static void handle_variant_args(struct variant_info *info, const char *key,
                                int key_len, char **dest, int *dest_len)
{
    if (!strncmp(key, "BANDWIDTH=", key_len)) {
        *dest     =        info->bandwidth;
        *dest_len = sizeof(info->bandwidth);
    }
}

struct key_info {
     char uri[MAX_URL_SIZE];
     char method[10];
     char iv[35];
};

static void handle_key_args(struct key_info *info, const char *key,
                            int key_len, char **dest, int *dest_len)
{
    if (!strncmp(key, "METHOD=", key_len)) {
        *dest     =        info->method;
        *dest_len = sizeof(info->method);
    } else if (!strncmp(key, "URI=", key_len)) {
        *dest     =        info->uri;
        *dest_len = sizeof(info->uri);
    } else if (!strncmp(key, "IV=", key_len)) {
        *dest     =        info->iv;
        *dest_len = sizeof(info->iv);
    }
}

static struct variant *new_variant(struct list_mgt *mgt, int bandwidth, const char *url, const char *base)
{
    struct variant *var = av_mallocz(sizeof(struct variant));
    if (!var)
        return NULL;
    
    var->bandwidth = bandwidth;
    ff_make_absolute_url(var->url, sizeof(var->url), base, url);
    //av_log(NULL,AV_LOG_INFO,"returl=%s\nbase=%s\nurl=%s\n",var->url,base,url);
    dynarray_add(&mgt->variants, &mgt->n_variants, var);
    return var;
}


static void free_variant_list(struct list_mgt *mgt)
{
    int i;
    for (i = 0; i < mgt->n_variants; i++) {
        struct variant *var = mgt->variants[i];        
        av_free(var);
    }
    av_freep(&mgt->variants);
    mgt->n_variants = 0;
}

#define TRICK_LOGIC_BASE 200
#ifndef INT_MAX
#define INT_MAX   2147483647
#endif
static int m3u_parser_line(struct list_mgt *mgt,unsigned char *line,struct list_item*item)
{
	unsigned char *p=line; 
	int enditem=0;
	const char* ptr = NULL;		
	while(*p==' ' && p!='\0' && p-line<1024) p++;
	if(*p!='#' && strlen(p)>0&&mgt->is_variant==0)
	{		
		item->file=p; 
		enditem=1;
	}else if(is_TAG(p,EXT_X_ENDLIST)){
		item->flags=ENDLIST_FLAG;		
		enditem=1;
	}else if(is_TAG(p,EXTINF)){
		int duration=0;
		sscanf(p+8,"%d",&duration);//skip strlen("#EXTINF:")
		if(duration>0){
			item->flags|=DURATION_FLAG;
			item->duration=duration;
			
		}
	} else if (av_strstart(line, "#EXT-X-TARGETDURATION:", &ptr)) {            	
		mgt->target_duration = atoi(ptr);
		av_log(NULL, AV_LOG_INFO, "get target duration:%ld\n",mgt->target_duration);
	}else if(is_TAG(p,EXT_X_ALLOW_CACHE)){
		item->flags|=ALLOW_CACHE_FLAG;
	}else if(is_TAG(p,EXT_X_MEDIA_SEQUENCE)){
		int seq = -1;
		int slen = strlen("#EXT-X-MEDIA-SEQUENCE:");
		sscanf(p+slen,"%d",&seq); //skip strlen("#EXT-X-MEDIA-SEQUENCE:");	
		if(seq>0){
			if(seq>mgt->seq){
				mgt->seq = seq;
				mgt->flags |=REAL_STREAMING_FLAG;
				if(mgt->cur_seq_no>=mgt->seq){
					mgt->jump_item_num = mgt->cur_seq_no - mgt->seq+1;
					av_log(NULL,AV_LOG_INFO,"need jump item num:%d\n",mgt->jump_item_num);
				}
				av_log(NULL, AV_LOG_INFO, "get new sequence number:%ld\n",seq);
			}else{
				av_log(NULL, AV_LOG_INFO, "drop this list,sequence number:%ld\n",seq);
				mgt->jump_item_num = INT_MAX;
				return 0;

			}
			
		}else{
			mgt->seq = seq;
			av_log(NULL, AV_LOG_INFO, "get a invalid sequence number:%d\n",seq);

		}
	}
	
	else if(av_strstart(p,"#EXT-X-STREAM-INF:",&ptr)){
		struct variant_info info = {{0}};           
		ff_parse_key_value(p, (ff_parse_key_val_cb) handle_variant_args,
		           &info);
		mgt->bandwidth = atoi(info.bandwidth);		
		av_log(NULL, AV_LOG_INFO, "get a stream info,bandwidth:%d\n",mgt->bandwidth);		
		mgt->is_variant = 1;
		return -(TRICK_LOGIC_BASE+1);
	}
	
	else if(av_strstart(p,"#EXT-X-KEY:",&ptr)){
		
		struct key_info info = {{0}};
		uint8_t iv[16] = "";
		ff_parse_key_value(ptr, (ff_parse_key_val_cb) handle_key_args,
		                   &info);
		#if 0
		av_log(NULL,AV_LOG_INFO,"==========start dump a aes key===========\n");
		av_log(NULL,AV_LOG_INFO,"==========key location : %s\n",info.uri);
		av_log(NULL,AV_LOG_INFO,"==========key iv : %s\n",info.iv);
		av_log(NULL,AV_LOG_INFO,"==========key method : %s\n",info.method);
		av_log(NULL,AV_LOG_INFO,"==========end dump a aes key===========\n");
		
		#endif
		struct encrypt_key_priv_t* key_priv_info = av_mallocz(sizeof(struct encrypt_key_priv_t));
		
		if(NULL == key_priv_info){
			av_log(NULL,AV_LOG_ERROR,"no memory for key_info\n");
			return -1;
		}		

		if(NULL!=mgt->key_tmp){
			av_log(NULL,AV_LOG_INFO,"released old key info\n");
			av_free(mgt->key_tmp);
			mgt->key_tmp = NULL;
		}
		
		enum KeyType key_type = KEY_NONE;
		if (!strcmp(info.method, "AES-128")){
		    	key_type = KEY_AES_128;
			
		}
		if (!strncmp(info.iv, "0x", 2) || !strncmp(info.iv, "0X", 2)) {
			ff_hex_to_data(iv, info.iv + 2);
			mgt->has_iv = 1;
		}else{
			mgt->has_iv = 0;
		}
		if(key_type ==KEY_AES_128){
			key_priv_info->key_type = key_type;
			if(mgt->has_iv>0){
				memcpy(key_priv_info->iv,iv,sizeof(key_priv_info->iv));
			}
			
			av_log(NULL,AV_LOG_INFO,"aes key location : %s\n",info.uri);
			av_strlcpy(key_priv_info->key_from, info.uri, sizeof(key_priv_info->key_from));
			key_priv_info->is_have_key_file = 0;
			mgt->key_tmp = key_priv_info;
			mgt->flags |= KEY_FLAG;
			return -(TRICK_LOGIC_BASE+0);
			
		}else{
			av_log(NULL,AV_LOG_INFO,"just only support aes key\n");
			av_free(key_priv_info);
			key_priv_info = NULL;
		}
		
		
	}
	else{

		if(mgt->is_variant>0){
			if(av_strstart(p,"http",&ptr)){
				if (!new_variant(mgt, mgt->bandwidth, p, NULL)) {	
					free_variant_list(mgt);
				 	return -1;
				}

			}else{
				if (!new_variant(mgt, mgt->bandwidth, p, mgt->prefix)) {	
					free_variant_list(mgt);
				 	return -1;
				}
			}
			mgt->is_variant = 0;
			mgt->bandwidth  = 0;	
			
			return -(TRICK_LOGIC_BASE+2);
			
		}
		
		return 0;
	}
	return enditem;
}


static int m3u_format_parser(struct list_mgt *mgt,ByteIOContext *s)
{ 
	unsigned  char line[1024];
	int ret;
	unsigned char *p; 
	int getnum=0;
	struct list_item tmpitem;
 	char prefix[1024]="";
	char prefixex[1024]="";
	int prefix_len=0,prefixex_len=0;
	int start_time=mgt->full_time;
	char *oprefix=mgt->location!=NULL?mgt->location:mgt->filename;
	if(NULL!=mgt->prefix){
		av_free(mgt->prefix);
		mgt->prefix = NULL;
	}	
	
	if(oprefix){
		mgt->prefix = strdup(oprefix);
		
		char *tail,*tailex,*extoptions;
		extoptions=strchr(oprefix,'?');/*ext options is start with ? ,we don't need in nested*/
		if(is_NET_URL(oprefix)){
			tail=strchr(oprefix+10,'/');/*skip Http:// and shttp:,and to first '/'*/
			if(!extoptions)// no ?
				tailex=strrchr(oprefix+10,'/');/*skip Http:// and shttp:,start to  last '/'*/
			else
				tailex=memrchr(oprefix+10,'/',extoptions-oprefix-10);/*skip Http:// and shttp:,start to  last '/',between http-->? */
		}else{
			tail=strchr(oprefix,'/'); /*first '/'*/
			if(!extoptions)//no ?
				tailex=strrchr(oprefix,'/'); /*to last '/' */
			else
				tailex=memrchr(oprefix+10,'/',extoptions-oprefix-10);/*skip Http:// and shttp:,start to  last '/',between http-->? */
		}
		
		if(tail!=NULL){
			prefix_len=tail-oprefix+1;/*left '/'..*/
			memcpy(prefix,oprefix,prefix_len);
			prefix[prefix_len]='\0';
		}

		if(tailex!=NULL){
			prefixex_len=tailex-oprefix+1;/*left '/'..*/
			memcpy(prefixex,oprefix,prefixex_len);
			prefixex[prefixex_len]='\0';	
		}
	}
	memset(&tmpitem,0,sizeof(tmpitem));
	av_log(NULL, AV_LOG_INFO, "m3u_format_parser get prefix=%s\n",prefix);
	av_log(NULL, AV_LOG_INFO, "m3u_format_parser get prefixex=%s\n",prefixex);
	#if 0
	if(mgt->n_variants>0){
		free_variant_list(mgt);
	}
	#endif
	while(m3u_format_get_line(s,line,1024)>=0)
	{
		ret = m3u_parser_line(mgt,line,&tmpitem);
		if(ret>0)
		{		
			struct list_item*item;
			int need_prefix=0;
			int size_file=tmpitem.file?(strlen(tmpitem.file)+32):4;
			tmpitem.start_time=start_time;
			
			if(tmpitem.file && 
				(is_NET_URL(prefix)) && /*net protocal*/
				!(is_NET_URL(tmpitem.file)))/*if item is not net protocal*/
			{/*if m3u is http,item is not http,add prefix*/
				need_prefix=1;
				size_file+=prefixex_len;
			}
			item=av_malloc(sizeof(struct list_item)+size_file);
			if(!item)
				return AVERROR(ENOMEM);
			memcpy(item,&tmpitem,sizeof(tmpitem));
			item->file=NULL;
			if(tmpitem.file)
			{
				item->file=&item[1];
				if(need_prefix){
					if(tmpitem.file[0]=='/'){/*has '/',not need the dir */
						strcpy(item->file,prefix);
						strcpy(item->file+prefix_len,tmpitem.file+1);/*don't copy two '/',we have left before*/
					}else{/*no '/', some I save the full path frefix*/
						if(!strncmp(prefixex,"shttps://",9)){
							strcpy(item->file,"http");
							strcpy(item->file+4,prefixex+6);
							strcpy(item->file+4+prefixex_len -6,tmpitem.file);
						}else{
							strcpy(item->file,prefixex);
							strcpy(item->file+prefixex_len,tmpitem.file);	
						}
						
					}
				}
				else{
					strcpy(item->file,tmpitem.file);
				}
			}

			if(mgt->flags&KEY_FLAG&&NULL!= mgt->key_tmp&&mgt->key_tmp->is_have_key_file>0){
				item->key_ctx = av_mallocz(sizeof(struct AES128KeyContext));
				if(!item->key_ctx){
					ret = AVERROR(ENOMEM);
					break;
				}				
				memcpy(item->key_ctx->key,mgt->key_tmp->key,sizeof(item->key_ctx->key));
				if(mgt->has_iv>0){
					memcpy(item->key_ctx->iv,mgt->key_tmp->iv,sizeof(item->key_ctx->iv));				
				}else{//from applehttp.c
					
					int seq = mgt->seq+mgt->item_num;
					av_log(NULL,AV_LOG_INFO,"Current item seq number:%d\n",seq);		
					AV_WB32(item->key_ctx->iv + 12, seq);
				}

				
				item->ktype = mgt->key_tmp->key_type;

			}
			if(mgt->flags&REAL_STREAMING_FLAG){			
				ret =list_test_and_add_item(mgt,item);
				if(ret==0){
					start_time+=item->duration;
				}
				
			}else{
				ret = list_add_item(mgt,item);
				start_time+=item->duration;

			}
			if(item->flags &ENDLIST_FLAG)
			{
				mgt->have_list_end=1;
				break;
			}
			else
			{
				memset(&tmpitem,0,sizeof(tmpitem));
				if(ret == 0){
					getnum++;

				}
			}
		}
		else if(ret <0){
			if(ret ==-(TRICK_LOGIC_BASE+0)&&(mgt->flags&KEY_FLAG)&&NULL!= mgt->key_tmp&&0==mgt->key_tmp->is_have_key_file){//get key from server
				 URLContext *uc;
			        if (ffurl_open(&uc, mgt->key_tmp->key_from, AVIO_FLAG_READ) == 0) {
			            if (ffurl_read_complete(uc, mgt->key_tmp->key, sizeof(mgt->key_tmp->key))
			                != sizeof(mgt->key_tmp->key)) {
			                av_log(NULL, AV_LOG_ERROR, "Unable to read key file %s\n",
			                       mgt->key_tmp->key_from);					
					
					}
					av_log(NULL,AV_LOG_INFO,"Just get aes key file from server\n");		
					mgt->key_tmp->is_have_key_file = 1;
						
					ffurl_close(uc);
			       } else {
			            	av_log(NULL, AV_LOG_ERROR, "Unable to open key file %s\n",
			                   mgt->key_tmp->key_from);
			       }
				memset(&tmpitem,0,sizeof(tmpitem));   
				continue;   
			}
			
			if(ret ==-(TRICK_LOGIC_BASE+1)||ret ==-(TRICK_LOGIC_BASE+2)){
				memset(&tmpitem,0,sizeof(tmpitem));   
				continue;
			}			
			break;
		}
		else{
			if(tmpitem.flags&ALLOW_CACHE_FLAG)
				mgt->flags|=ALLOW_CACHE_FLAG;
			if(mgt->flags&REAL_STREAMING_FLAG&&mgt->jump_item_num==INT_MAX){
				mgt->jump_item_num = 0;
				av_log(NULL, AV_LOG_INFO, "drop this list,sequence number:%ld\n",mgt->seq);
				break;
			}
		}
		
	}
	if(mgt->key_tmp){
		av_free(mgt->key_tmp);
		mgt->key_tmp = NULL;
	}
	if(mgt->n_variants>0){//just choose  middle definition;
 		float value = 0.0;
		ret=am_getconfig_float("libplayer.hls.level",&value);
		if(ret<0){
			mgt->ctype =MIDDLE_BANDWIDTH;
		}else if(value<2){
			mgt->ctype =LOW_BANDWIDTH;
		}else if(value<3){
			mgt->ctype =MIDDLE_BANDWIDTH;
		}else if(value<4){
			mgt->ctype =HIGH_BANDWIDTH;
		}
	}
	mgt->file_size=AVERROR_STREAM_SIZE_NOTVALID;
	mgt->full_time=start_time;
	mgt->last_load_time = av_gettime();
	av_log(NULL, AV_LOG_INFO, "m3u_format_parser end num =%d,fulltime=%d\n",getnum,start_time);
	return getnum;
}

static int match_ext(const char *filename, const char *extensions)//get file type, .vob,.mp4,.ts...
{
    const char *ext, *p;
    char ext1[32], *q;

    if(!filename)
        return 0;

    ext = strrchr(filename, '.');
    if (ext) {
        ext++;
        p = extensions;
        for(;;) {
            q = ext1;
            while (*p != '\0' && *p != ',' && q-ext1<sizeof(ext1)-1)
                *q++ = *p++;
            *q = '\0';
            if (!strcasecmp(ext1, ext))
                return 1;
            if (*p == '\0')
                break;
            p++;
        }
    }
    return 0;
}

static int m3u_probe(ByteIOContext *s,const char *file)
{
	if(s)
	{
		char line[1024];
		if(m3u_format_get_line(s,line,1024)>0)
		{

			if(memcmp(line,EXTM3U,strlen(EXTM3U))==0)
			{				
				av_log(NULL, AV_LOG_INFO, "get m3u flags!!\n");
				return 100;
			}
		}	
	}
	else
	{
		if((match_ext(file, "m3u"))||(match_ext(file, "m3u8"))) 
		{
			return 50;
		}
	}
	return 0;
}
 
 struct list_demux m3u_demux = {
	"m3u",
    m3u_probe,
	m3u_format_parser,
};



