
/**
 * @defgroup 
 * @file 
 * @brief 
 * @author 草台班子
 * @version 
 * @date 2024年10月7日
 */

#include "OverlayManager.h"
#include "TlErrnoPublic.h"
#include "TlLogPublic.h"
#include "DebugAgent.h"
#include "global.h"
#include "Media.h"

#include "MODev.h"
#include "typedef.h"
#include "osal.h"
#include "osal_mutex.h"
#include "osal_mem.h"
#include "osal_tsk.h"
#include "doublelink.h"
#include "CommQue.h"
#include "../MO/MOJsonRpc.h"
#include "StbpClient.h"
#include "JsonParse.h"
#include "MediaMsg.h"
#include "rkSpbo.h"
#include "ShareMemQue.h"
#include "TlCommonFunc.h"
#include "RkMppFrmEx.h"
#include "rkscale.h"
#include "rkframe.h"
#include "RkMppFrm.h"

#define BannerCacheNum    3
#define BannerCacheNum_    3
#define CH_NUM    2
#define DEV_NUM    2
#define DEV_NUM_ERR    10
#define BannerCacheNum_MAX    10
#define RGA_MAX_width    8128



extern UINT32		MODevDebuglv;

typedef enum __DevIndex 
{
	HDMIOUT_1 = 0,
	HDMIOUT_2,
}DevIndex;

typedef struct
{
//	struct sp_bo *tempBo[BannerCacheNum_MAX];/*滚动横幅画布*/

	MppFrame tempBo[BannerCacheNum_MAX];/*滚动横幅画布*/
	UINT32  count;
//	struct sp_bo *tempBo_2;/*滚动横幅画布*/

	UINT32 width;
	UINT32 height;
	
	ShareMemQueID		hImgQue;/*滚动横幅共享画布*/
	String256		strId;/*共享内存id*/
	BOOL bEnable;/*是否准备好*/
}T_BannerCanvas;

typedef struct
{
	DECLARELISTNODE();
	T_DisplayBanner	tBanner;
	UINT32 	progressTimestamp;//记录偏移

	BOOL    enable;
	UINT32 	optTimestamp;//定时戳停止,尽量此时戳前停止

	BOOL 	bReport;
	UINT32 	reportTimestamp;//上报时戳

	BOOL 	bRepair;
	UINT32 	VOTimestamp;//输出修正时戳
	UINT32 	SrcTimestamp;//渲染修正时戳

	T_MutexObj  tCtrlLock;
	
	T_BannerCanvas tCanvas;//滚动画布
	
	UINT32 correct_offset;//纠正偏移
	
	UINT32  offset;//实时偏移
	
	BOOL Bstop_scroll;//是否开始滚动
		
}T_BannerNode;

typedef struct 
{
	UINT32 VoId;
	UINT32 plane;/*图层索引号*/
	
	INT32 chId;/*当前横幅服务传递的共享内存服务于哪个通道*/

	T_Rectex dirty_rect;/*当前横幅服务传递的共享内存的脏矩阵*/

	struct sp_bo *bo[BannerCacheNum];/*真正送显帧,注册帧*/
	int use_bo;

	struct sp_bo *bo_share[OverlayCacheNum];/*分享给其他进程使用帧*/
	
	BOOL enable;
}T_BannerPlane;

/** @brief 管理 */
typedef struct 
{
	BOOL bState;

//	TSK_Handle	tPlayTsk; /*数据输出*/
	BOOL bThrdRun;
	
	TSK_Handle	tCh0MergeTsk; /*通道内容整合*/
	BOOL bThrdCh0MergeRun;

	TSK_Handle	tCh1MergeTsk; /*通道内容整合*/
	BOOL bThrdCh1MergeRun;

	TSK_Handle	tCopyPlayTsk; /*通道内容copy*/
	BOOL bThrdCopyPlayRun;

	CommQueID hCh0Que;//通道0
	CommQueID hCh1Que;//通道1
	
	CommQueID hQue_copy;//vblank驱动数据准备和输出

	T_MutexObj  tMutex;
	
	T_StdListDef tBannerList;  /**< 横幅列表 */
	T_MutexObj  tBannerMutex;
	
	UINT32 used_index;/*当前横幅服务传递的共享内存的索引号*/

	struct sp_bo *bo_ch[CH_NUM][BannerCacheNum_];/*通道临时数据*/
	int use_bo[CH_NUM];

//	T_BannerPlane tPlane0;
//	T_BannerPlane tPlane1;
	T_BannerPlane tPlane[DEV_NUM];

	MppBufferGroup pBufferGrp;

} T_Manage;

/** @brief 管理 */
static T_Manage *s_pManage = NULL;
static T_BannerNode* OverlayFindBannerNode(	UINT32	uiChId, UINT32	uiPlayId);
static E_StateCode freeALLScrollBannerMem();

static T_Manage* OverlayManager(void) {
  return s_pManage;
}

static int VBlankCb(int VoId, unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec, void *pUserData)
{
	T_Manage	*ptObj = (T_Manage *)pUserData;
	void *ptCmd = NULL;
	if (NULL == ptObj)
	{
		return SMP_FAIL;
	}
	
	ptCmd = CommQue_GetEmpty(ptObj->hQue_copy, OSAL_TIMEOUT_NONE);
	if (ptCmd != NULL)
	{
		CommQue_PutFull(ptObj->hQue_copy, ptCmd);
	}
	else
	{
		CommQue_Clear(ptObj->hQue_copy); 
	}
	return SMP_OK;
}

static E_StateCode OverlayManagerPlay(T_Overlay* playinfo);
//VOID OverlayPlay_TskCb(VOID *param)
//{
//  T_Manage *ptObj = (T_Manage *)param;
////  void *ptCmd = NULL;
//  T_Overlay *playinfo = NULL;
//  int ret = 0;
//  ptObj->bThrdRun = SMP_TRUE; 
//  TllSleep(1);
//  ret = MODevRegisterGlobalVblank(VBlankCb, ptObj);
//  dbprintf("MODevRegisterVblank, iRet = %d\n",  ret);
//  MODevUnRegisterGlobalVblank(VBlankCb, ptObj);
//  ptObj->bThrdRun = SMP_FALSE;
//}


/**********************************************************************
* 函数名称：addtimestamp
* 功能描述：整合时戳
* 输入参数：T_Overlay， T_OverlayMergeInfo
* 输出参数：无
* 返 回 值：	 状态码
* 其它说明： 
* 修改日期        版本号     修改人	      修改内容
* -----------------------------------------------
* 2024/12/27	     V1.0	           chengjiahao
***********************************************************************/
static void addtimestamp(T_Overlay *ptMergeInfo, T_OverlayMergeInfo * chInfo)
{
	int i = 0;
	int index = 0;
	for(i = 0; i < BANNER_NUM; i++)
	{
		index = chInfo->uiChId * BANNER_NUM + i;
		ptMergeInfo->uiTimestamp_draw[index] = chInfo->uiTimestamp_draw[i];	
	}	
	
}

/**********************************************************************
* 函数名称：BannerCopy
* 功能描述：通道到设备的内容拷贝
* 输入参数：T_Overlay
* 输出参数：无
* 返 回 值：	 状态码
* 其它说明： 
* 修改日期        版本号     修改人	      修改内容
* -----------------------------------------------
* 2024/12/27	     V1.0	           chengjiahao
***********************************************************************/
static E_StateCode BannerCopy(void *param, T_Overlay *ptMergeInfo)
{
	T_Manage * ptObj = (T_Manage * )param;
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	struct sp_bo *dst = NULL; 
	struct sp_bo *src = NULL;
	BOOL bNeedch1 =FALSE;
	T_OverlayMergeInfo * bo_ch0Info = NULL;
	T_OverlayMergeInfo * bo_ch1Info = NULL;
	int i = 0;

	if(ptObj->tPlane[0].chId == 1 || ptObj->tPlane[1].chId == 1)
	{
		bNeedch1 = TRUE;
	}

	OSAL_MutexLock(&ptObj->tMutex);

	bo_ch0Info = (T_OverlayMergeInfo *)CommQue_GetFull(ptObj->hCh0Que, 100);
	if(bo_ch0Info == NULL)
	{
		SysErr("CommQue_GetFull filed \n");
	}
	
	if(bNeedch1)
	{
		bo_ch1Info = (T_OverlayMergeInfo *)CommQue_GetFull(ptObj->hCh1Que, 100);
		if(bo_ch1Info == NULL)
		{
			SysErr("CommQue_GetFull filed \n");
		}
	}
	
	if(bo_ch0Info == NULL && bo_ch1Info == NULL)
	{
		eCode = STATE_CODE_OBJECT_BUSY;	
		goto end;
	}	


	for(i = 0; i < DEV_NUM; i++)
	{
		dst = NULL;
		dst = ptObj->tPlane[i].bo[ptObj->tPlane[i].use_bo];
		if(dst != NULL)
		{
			if(ptObj->tPlane[i].chId == 0 && bo_ch0Info != NULL)
			{
				src = ptObj->bo_ch[0][bo_ch0Info->index];
				T_Rectex src_r = {0, 0, src->width, src->height};
				copy_bo_fd(dst, src, &src_r, 0, 0);
				addtimestamp(ptMergeInfo, bo_ch0Info);
			}
			else if(ptObj->tPlane[i].chId == 1 && bo_ch1Info != NULL)
			{
				src = ptObj->bo_ch[1][bo_ch1Info->index];
				T_Rectex src_r = {0, 0, src->width, src->height};
				copy_bo_fd(dst, src, &src_r, 0, 0);
				addtimestamp(ptMergeInfo, bo_ch1Info);
			}
			else
			{
				fill_bo(dst, 0, 0, 0, 0);
			}	
		}		
	}
end:

	if(bo_ch0Info != NULL)
	{
		CommQue_PutEmpty(ptObj->hCh0Que, bo_ch0Info);
	}
	if(bo_ch1Info != NULL)
	{
		CommQue_PutEmpty(ptObj->hCh1Que, bo_ch1Info);
	}

	OSAL_MutexUnlock(&ptObj->tMutex);
	return eCode;
}

void BannerCopy_Cb(void *param)
{
  T_Manage * ptObj = (T_Manage * )param;

  T_Overlay temp;
  T_Overlay *ptCmd = NULL;
  ptCmd = &temp;
  memset(ptCmd, 0x00, sizeof(T_Overlay));
  void *ptTemp = NULL;
  ptObj->bThrdCopyPlayRun = SMP_TRUE;
  E_StateCode eCode = STATE_CODE_NO_ERROR;
  TllSleep(1);

  while (ptObj->bThrdCopyPlayRun) 
  {
  	  ptTemp = (void *)CommQue_GetFull(ptObj->hQue_copy, 100);
	  if(NULL != ptTemp)
	  {	
	  	  eCode = BannerCopy(param, ptCmd);
		  if(STATE_OK(eCode))
		  {
		  	ptCmd->index = ptObj->tPlane[0].use_bo;
		  	ptObj->tPlane[0].use_bo = (ptObj->tPlane[0].use_bo + 1)%BannerCacheNum;
		  	ptObj->tPlane[1].use_bo = (ptObj->tPlane[1].use_bo + 1)%BannerCacheNum;
			eCode = OverlayManagerPlay(ptCmd);
			if(eCode != 0)
			{
				SysErr("rkVo_SetPlaneForOverlay err\n");
			}
		  }
		  CommQue_PutEmpty(ptObj->hQue_copy, ptTemp);
	  }
	  else
	  {
	  	TSK_sleep(2);	
	  }

  }
  ptObj->bThrdCh0MergeRun = SMP_FALSE;
}

/**********************************************************************
* 函数名称：double_merge
* 功能描述：多内存组合拷贝
* 输入参数：
* 输出参数：无
* 返 回 值：	 状态码
* 其它说明： 
* 修改日期        版本号     修改人	      修改内容
* -----------------------------------------------
* 2024/12/27	     V1.0	           chengjiahao
***********************************************************************/
static void  batch_copy(T_BannerNode* ptNode, UINT8 *pucSrc)
{
	INT8 *pucDst = NULL;
	UINT8 *pucSrc_temp = NULL;
	int  s_width = 0;

	MppBuffer buffer = NULL;
//	buffer = mpp_frame_get_buffer(ptNode->tCanvas.tempBo[0]);
//	pucDst = mpp_buffer_get_ptr(buffer);

	
	s_width = ptNode->tCanvas.width -  RGA_MAX_width * (ptNode->tCanvas.count - 1);

	int i = 0;
	int j = 0;	
	for (i = 0; i < ptNode->tCanvas.count - 1; i++)
	{
		pucSrc_temp = pucSrc;
		buffer = mpp_frame_get_buffer(ptNode->tCanvas.tempBo[i]);
		pucDst = mpp_buffer_get_ptr(buffer);
//		pucDst = ptNode->tCanvas.tempBo[i]->map_addr;
		pucSrc_temp += RGA_MAX_width * 4 * i; 
		for (j = 0; j < ptNode->tCanvas.height; j++)
		{
			memcpy(pucDst, pucSrc_temp, RGA_MAX_width * 4);
			pucDst += (RGA_MAX_width * 4);
			pucSrc_temp += (ptNode->tCanvas.width * 4);
		}
	}

	buffer = mpp_frame_get_buffer(ptNode->tCanvas.tempBo[ptNode->tCanvas.count - 1]);
	pucDst = mpp_buffer_get_ptr(buffer);

//	pucDst = ptNode->tCanvas.tempBo[ptNode->tCanvas.count - 1]->map_addr;
	pucSrc_temp = pucSrc;
	pucSrc_temp += RGA_MAX_width * 4 * (ptNode->tCanvas.count - 1);
	for (j = 0; j < ptNode->tCanvas.height; j++)
	{
		memcpy(pucDst, pucSrc_temp, s_width * 4);
		pucDst += (RGA_MAX_width * 4);
		pucSrc_temp += (ptNode->tCanvas.width * 4);
	}

}

static UINT32 changeArray[BannerCacheNum_MAX -1] = {RGA_MAX_width, RGA_MAX_width*2, RGA_MAX_width*3, \
		RGA_MAX_width*4, RGA_MAX_width*5, RGA_MAX_width*6, \
		RGA_MAX_width*7, RGA_MAX_width*8, RGA_MAX_width*9};

BOOL isNeedDoubleSrc(int Crop_left, int Crop_right, int count) 
{
	BOOL bNeed = FALSE;
	int i = 0;
	UINT32 midNum = 0;
	for(i = 1; i < count; i++)
	{
		midNum = changeArray[i -1];
		if(midNum > Crop_left &&  midNum <= Crop_right)
		{
			bNeed = TRUE;
			break;
		}
	}
    return bNeed;
}

static void FillSpBoFromFrame(struct sp_bo *bo, MppFrame frame) 
{
	MppBuffer buffer = NULL;
	if(!bo || frame == NULL)
	{
		return;
	}
	buffer = mpp_frame_get_buffer(frame);
	bo->fd = mpp_buffer_get_fd(buffer);
	bo->width = mpp_frame_get_width(frame);
	bo->height = mpp_frame_get_height(frame);
}


/**********************************************************************
* 函数名称：double_merge
* 功能描述：多内存组合merge
* 输入参数：
* 输出参数：无
* 返 回 值：	 状态码
* 其它说明： 
* 修改日期        版本号     修改人	      修改内容
* -----------------------------------------------
* 2024/12/27	     V1.0	           chengjiahao
***********************************************************************/

static void  double_merge(struct sp_bo *dst, T_BannerNode* ptNode, float changeX, float changeY)
{
	MppFrame src = NULL;
	MppFrame src_l = NULL;
	MppFrame src_r = NULL;
//	MppBuffer buffer = NULL;
	struct sp_bo src_copy;
	memset(&src_copy, 0x00, sizeof(src_copy));
	struct sp_bo src_l_copy;
	memset(&src_l_copy, 0x00, sizeof(src_l_copy));
	struct sp_bo src_r_copy;
	memset(&src_r_copy, 0x00, sizeof(src_r_copy));
	
	int count = 0;
	int index = 0;
	int Crop_left = 0;
	int Crop_right = 0;
	T_DisplayBanner *stInfo = &ptNode->tBanner;
	int rect_y = stInfo->generateRect.top * changeY;//外部提供

	int show_x = stInfo->showRect.left * changeX;//外部提供显示坐标
	int show_y = stInfo->showRect.top * changeY;//外部提供
	
	int show_w = (stInfo->showRect.right - stInfo->showRect.left) * changeX;//外部提供
	int show_h = (stInfo->showRect.bottom - stInfo->showRect.top) * changeY;//外部提供
	int show_all = stInfo->totalWidth * changeX;//外部提供 显示宽度

	int all = ptNode->tCanvas.width + show_all;
	int offset_x = ptNode->offset % all;

	int Crop_width = ptNode->tCanvas.width - offset_x;
	
	count = ptNode->tCanvas.count;

	if(Crop_width >= show_w)
	{
		Crop_left = offset_x;
		Crop_right = offset_x + show_w;
		
		if(isNeedDoubleSrc(Crop_left, Crop_right, count))
		{
			index = Crop_left / RGA_MAX_width;
			src_l =  ptNode->tCanvas.tempBo[index];
			src_r =  ptNode->tCanvas.tempBo[index + 1];

			FillSpBoFromFrame(&src_l_copy, src_l);
			FillSpBoFromFrame(&src_r_copy, src_r);
			int temp_w = RGA_MAX_width - (offset_x % RGA_MAX_width);
	
			T_Rectex src_rect_l = {(offset_x % RGA_MAX_width), rect_y, temp_w, show_h};

			T_Rectex src_rect_r = {0, rect_y, show_w - temp_w, show_h};

			T_Rectex dst_rect_l = {show_x, show_y, show_w, show_h};	
			
			copy_bo_fd(dst, &src_l_copy, &src_rect_l, dst_rect_l.x, dst_rect_l.y);
			

			int temp_x = show_x + temp_w;
			
			T_Rectex dst_rect_r = {temp_x , show_y, show_w, show_h};	
			
			copy_bo_fd(dst, &src_r_copy, &src_rect_r, dst_rect_r.x, dst_rect_r.y);

		}
		else
		{
			index = Crop_left / RGA_MAX_width;
			src =  ptNode->tCanvas.tempBo[index];

			FillSpBoFromFrame(&src_copy, src);

			T_Rectex src_r = {(offset_x % RGA_MAX_width), rect_y, show_w, show_h};
			T_Rectex dst_r = {show_x, show_y, show_w, show_h};
			copy_bo_fd(dst, &src_copy, &src_r, dst_r.x, dst_r.y);	
		}
	
	}
	else if(Crop_width >= 0)
	{
		int s_width = ptNode->tCanvas.width -  RGA_MAX_width * (count - 1);//尾部内存有效宽度
		Crop_left = offset_x;
		Crop_right = offset_x + Crop_width;


		if(s_width >= Crop_width)
		{
			index = Crop_left / RGA_MAX_width;
			src =  ptNode->tCanvas.tempBo[index];

			FillSpBoFromFrame(&src_copy, src);

	
			T_Rectex src_r = {(offset_x % RGA_MAX_width), rect_y, Crop_width, show_h};
			
			T_Rectex dst_r = {show_x, show_y, Crop_width, show_h};
			
			copy_bo_fd(dst, &src_copy, &src_r, dst_r.x, dst_r.y);	
		}
		else if(isNeedDoubleSrc(Crop_left, Crop_right, count))
		{
			index = Crop_left / RGA_MAX_width;
			src_l =  ptNode->tCanvas.tempBo[index];
			src_r =  ptNode->tCanvas.tempBo[index + 1];

			FillSpBoFromFrame(&src_l_copy, src_l);
			FillSpBoFromFrame(&src_r_copy, src_r);

			int temp_w = RGA_MAX_width - (offset_x % RGA_MAX_width);
	
			T_Rectex src_rect_l = {(offset_x % RGA_MAX_width), rect_y, temp_w, show_h};

			T_Rectex src_rect_r = {0, rect_y, Crop_width - temp_w, show_h};

			T_Rectex dst_rect_l = {show_x, show_y, Crop_width, show_h};	
			copy_bo_fd(dst, &src_l_copy, &src_rect_l, dst_rect_l.x, dst_rect_l.y);	

			int temp_x = show_x + temp_w;
			T_Rectex dst_rect_r = {temp_x , show_y, show_w, show_h};	
			copy_bo_fd(dst, &src_r_copy, &src_rect_r, dst_rect_r.x, dst_rect_r.y);	
		}

	}

	if(Crop_width < 0)
	{
		Crop_width = -Crop_width;
		if(Crop_width <= show_all - show_w)
		{

		}
		else if (Crop_width > show_all - show_w)
		{
//			index = Crop_left / RGA_MAX_width;
			src =  ptNode->tCanvas.tempBo[0];

			FillSpBoFromFrame(&src_copy, src);

			int offset = Crop_width - (show_all - show_w);
			T_Rectex src_r = {0, rect_y, offset, show_h};
			T_Rectex dst_r = {show_w + show_x - offset, show_y, offset, show_h};
			copy_bo_fd(dst, &src_copy, &src_r, dst_r.x, dst_r.y);
		}	
	}
}

/**********************************************************************
* 函数名称：BannerCopy
* 功能描述：通道内容整合
* 输入参数：T_Overlay
* 输出参数：无
* 返 回 值：	 状态码
* 其它说明： 
* 修改日期        版本号     修改人	      修改内容
* -----------------------------------------------
* 2024/12/27	     V1.0	           chengjiahao
***********************************************************************/
static int BannerMerge(T_Manage *ptObj, UINT32 uiChId, UINT32 uidevId,T_OverlayMergeInfo* info)
{
	int i = 0;
	int j = 0;
	int ret = 0;
	int index = 0;
	T_BannerNode* ptNode = NULL;
	float changeX = 0.0, changeY = 0.0;
	struct sp_bo *dst = NULL; 
	MppFrame src = NULL;
	struct sp_bo *pat = NULL;
	T_Rectex *dirty_rect = NULL;

	int all = 0;
	int show_all = 0;

	OSAL_MutexLock(&ptObj->tMutex);

//	pat =  rkVo_GetPlaneForOverlay(ptObj->tPlane[uidevId].VoId, ptObj->tPlane[uidevId].plane, ptObj->used_index);
	pat = ptObj->tPlane[uidevId].bo_share[ptObj->used_index];
	dirty_rect = &ptObj->tPlane[uidevId].dirty_rect;
	
	if(pat == NULL)
	{
		info->uiChId = DEV_NUM_ERR;
		OSAL_MutexUnlock(&ptObj->tMutex);	
		return 0;
	}

	ret = ptObj->use_bo[uiChId];
	dst =  ptObj->bo_ch[uiChId][ptObj->use_bo[uiChId]];

	fill_bo(dst, 0, 0, 0, 0);

	T_Rectex pat_r = {0, 0, dst->width, dst->height};
	
	if(dirty_rect->w > 0 && dirty_rect->w <= dst->width && dirty_rect->x >= 0)
	{
		pat_r.x = dirty_rect->x;
		pat_r.y = dirty_rect->y;
		pat_r.w = dirty_rect->w;
		pat_r.h = dirty_rect->h;
		copy_bo_fd(dst, pat, &pat_r,  pat_r.x, pat_r.y);
	}
	OSAL_MutexUnlock(&ptObj->tMutex);

	/*copy*/
	for(i = 0; i <BANNER_NUM; i++)
	{
		ptNode = NULL;
		UINT8		*pucData = NULL;
		ptNode =  OverlayFindBannerNode(uiChId, i);
		if(!ptNode)
		{
			continue;
		}
		
		OSAL_MutexLock(&ptNode->tCtrlLock);
		if(NULL == ptNode->tCanvas.hImgQue)
		{
			OSAL_MutexUnlock(&ptNode->tCtrlLock);
			continue;	
		}

		pucData = ShareMemQue_GetReadPtr(ptNode->tCanvas.hImgQue);
		if(!pucData || ptNode->tCanvas.count == 0)
		{
			OSAL_MutexUnlock(&ptNode->tCtrlLock);
			continue;
		}

		if(ptNode->tCanvas.count > 1)
		{
			batch_copy(ptNode, pucData);
		}
		else
		{
			INT8 *pucDst = NULL;
			MppBuffer buffer = NULL;
			buffer = mpp_frame_get_buffer(ptNode->tCanvas.tempBo[0]);
			pucDst = mpp_buffer_get_ptr(buffer);
			for (j = 0; j < ptNode->tCanvas.height; j++)
			{
				memcpy(pucDst, pucData, ptNode->tCanvas.width * 4);
				pucDst += (ptNode->tCanvas.width * 4);
				pucData += (ptNode->tCanvas.width * 4);
			}
		}
		ptNode->tCanvas.bEnable = TRUE;
		ShareMemQue_PutReadPtr(ptNode->tCanvas.hImgQue);
		OSAL_MutexUnlock(&ptNode->tCtrlLock);
	}

	/*merge*/
	for(i = 0; i <BANNER_NUM; i++)
	{
		ptNode = NULL;
		ptNode =  OverlayFindBannerNode(uiChId, i);

		if(!ptNode)
		{
			continue;
		}
		
		OSAL_MutexLock(&ptNode->tCtrlLock);
		
		T_DisplayBanner * stInfo = &ptNode->tBanner;
		if(!stInfo->bannerText.enable || ptNode->Bstop_scroll || 0 == ptNode->tCanvas.count || !ptNode->tCanvas.bEnable)
		{
			OSAL_MutexUnlock(&ptNode->tCtrlLock);
			continue;	
		}

		changeX = (float)TYPICAL_WIDTH / stInfo->Max_rect.uiHor;
		changeY = (float)TYPICAL_HEIGHT / stInfo->Max_rect.uiVer;
		show_all = stInfo->totalWidth * changeX;//外部提供 显示宽度
		all = ptNode->tCanvas.width + show_all;
		if(ptNode->tCanvas.count == 1)
		{
			src =  ptNode->tCanvas.tempBo[0];
			struct sp_bo src_copy;
			memset(&src_copy, 0x00, sizeof(src_copy));
			FillSpBoFromFrame(&src_copy, src);
			
			//int rect_x = stInfo->generateRect.left * changeX;//外部提供裁剪坐标启动点
			int rect_y = stInfo->generateRect.top * changeY;//外部提供

			int show_x = stInfo->showRect.left * changeX;//外部提供显示坐标
			int show_y = stInfo->showRect.top * changeY;//外部提供
			
			int show_w = (stInfo->showRect.right - stInfo->showRect.left) * changeX;//外部提供
			int show_h = (stInfo->showRect.bottom - stInfo->showRect.top) * changeY;//外部提供
			
			int offset_x = ptNode->offset % all;

			
			int Crop_width = src_copy.width - offset_x;

			if(Crop_width >= show_w)
			{
				T_Rectex src_r = {offset_x, rect_y, show_w, show_h};
				T_Rectex dst_r = {show_x, show_y, show_w, show_h};

				if(!stInfo->bannerBg.isPureColorBg && stInfo->bannerBg.enable)
				{
					blend_bo(dst, &src_copy,  &dst_r,	&src_r);
				}
				else
				{
					copy_bo_fd(dst, &src_copy, &src_r, dst_r.x, dst_r.y);	
//					blend_bo(dst, src,  &dst_r,	&src_r);
				}
				
			}
			else if(Crop_width >= 0)
			{

				T_Rectex src_r = {offset_x, rect_y, Crop_width, show_h};
				T_Rectex dst_r = {show_x, show_y, Crop_width, show_h};
				if(!stInfo->bannerBg.isPureColorBg && stInfo->bannerBg.enable)
				{
					blend_bo(dst, &src_copy,  &dst_r,	&src_r);
				}
				else
				{
					copy_bo_fd(dst, &src_copy, &src_r, dst_r.x, dst_r.y);
//					blend_bo(dst, src,  &dst_r,	&src_r);
				}
			}

			if(Crop_width < 0)
			{
				Crop_width = -Crop_width;
				if(Crop_width <= show_all - show_w)
				{

				}
				else if (Crop_width > show_all - show_w)
				{
					int offset = Crop_width - (show_all - show_w);
					T_Rectex src_r = {0, rect_y, offset, show_h};
					T_Rectex dst_r = {show_w + show_x - offset, show_y, offset, show_h};
					if(offset > src_copy.width)
					{
						src_r.w = src_copy.width;
						dst_r.w = src_copy.width;
					}
					if(!stInfo->bannerBg.isPureColorBg && stInfo->bannerBg.enable)
					{
						blend_bo(dst, &src_copy,	&dst_r, &src_r);
					}
					else
					{
						copy_bo_fd(dst, &src_copy, &src_r, dst_r.x, dst_r.y);
					}

				}	
			}
		}
		else
		{
			double_merge(dst, ptNode, changeX, changeY);
		}
	
		UINT32 speed = stInfo->bannerText.scrollSpeed;
		
		ptNode->offset += speed;
		ptNode->offset += ptNode->correct_offset;
		ptNode->correct_offset = 0;
		index =  i;
		int fps = rkVo_GetFPS(ptObj->tPlane[0].VoId);
		info->uiTimestamp_draw[index] = (ptNode->offset + all - (stInfo->generateRect.left* changeX)) / speed * (1000/fps);
		OSAL_MutexUnlock(&ptNode->tCtrlLock);
	}	
	info->index = ptObj->use_bo[uiChId];
	info->uiChId = uiChId;
	ptObj->use_bo[uiChId] = (ptObj->use_bo[uiChId]+1)%BannerCacheNum_;
	return ret;
}

void BannerCh0Merge_Tsk_Cb(VOID *param)
{
  T_Manage * ptObj = (T_Manage * )param;
  T_OverlayMergeInfo *ptCmd = NULL;
  ptObj->bThrdCh0MergeRun = SMP_TRUE;

  TllSleep(1);
  while (ptObj->bThrdCh0MergeRun) 
  {
	  ptCmd = (T_OverlayMergeInfo *)CommQue_GetEmpty(ptObj->hCh0Que, 100);
	  if (NULL != ptCmd)
	  {
		  if(ptObj->tPlane[0].chId == 0 && ptObj->tPlane[0].enable)
		  {
			  BannerMerge(ptObj, ptObj->tPlane[0].chId, ptObj->tPlane[0].VoId, ptCmd);
		  }
		  else if(ptObj->tPlane[1].chId == 0 && ptObj->tPlane[1].enable)
		  {
			  BannerMerge(ptObj, ptObj->tPlane[1].chId, ptObj->tPlane[1].VoId, ptCmd);
		  }
		  else
		  {
			  ptCmd->uiChId = DEV_NUM_ERR;
		  }
		  CommQue_PutFull(ptObj->hCh0Que, ptCmd);
	  }
	   else   
	   {
//	   	  SysWarn("CommQue_GetEmpty filed \n");
		  CommQue_Clear(ptObj->hCh0Que); 
		  TSK_sleep(2);
	   }
  }
  ptObj->bThrdCh0MergeRun = SMP_FALSE;
}

void BannerCh1Merge_Tsk_Cb(VOID *param)
{
  T_Manage * ptObj = (T_Manage * )param;
  T_OverlayMergeInfo *ptCmd = NULL;
  ptObj->bThrdCh1MergeRun = SMP_TRUE;

  TllSleep(1);
  while (ptObj->bThrdCh1MergeRun) 
  {
	  ptCmd = (T_OverlayMergeInfo *)CommQue_GetEmpty(ptObj->hCh1Que, 100);
	  if (NULL != ptCmd)
	  {
		  if(ptObj->tPlane[0].chId == 1 && ptObj->tPlane[0].enable)
		  {
			  BannerMerge(ptObj, ptObj->tPlane[0].chId, ptObj->tPlane[0].VoId, ptCmd);
		  }
		  else if(ptObj->tPlane[1].chId == 1 && ptObj->tPlane[1].enable)
		  {
			  BannerMerge(ptObj, ptObj->tPlane[1].chId, ptObj->tPlane[1].VoId, ptCmd);	  
		  }
		  else
		  {
			  ptCmd->uiChId = DEV_NUM_ERR;	
		  }
		  CommQue_PutFull(ptObj->hCh1Que, ptCmd);
	  }
	   else   
	   {
//	   	  SysErr("CommQue_GetEmpty filed \n");
		  CommQue_Clear(ptObj->hCh1Que); 
		  TSK_sleep(2);
	   }

  }
  ptObj->bThrdCh0MergeRun = SMP_FALSE;
}

int OverlayManager_ModInit() 
{
	int i=  0;
	int j = 0;
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_Manage *ptObj = OverlayManager();
    if (ptObj != NULL) return 0;

	struct sp_dev *dev = NULL;
	dev = rkVo_GetSpDev(0);
    ptObj = (T_Manage *)malloc(sizeof(T_Manage));
    if (!ptObj) return -1;

	memset(ptObj, 0x0, sizeof(T_Manage));

	s_pManage = ptObj;

	ptObj->hQue_copy = CommQue_Create(3, sizeof(int), NULL);
	if (NULL == ptObj->hQue_copy)
	{
		SysErr("Create data que failed!\n");
		goto cleanup;
	}

	ptObj->hCh0Que = CommQue_Create(BannerCacheNum_ -1, sizeof(T_OverlayMergeInfo), NULL);
	if (NULL == ptObj->hCh0Que)
	{
		SysErr("Create data que failed!\n");
		goto cleanup;
	}
	ptObj->hCh1Que = CommQue_Create(BannerCacheNum_ -1, sizeof(T_OverlayMergeInfo), NULL);
	if (NULL == ptObj->hCh1Que)
	{
		SysErr("Create data que failed!\n");
		goto cleanup;
	}

	StdListInit(&ptObj->tBannerList);
	OSAL_MutexInit(&ptObj->tBannerMutex);
	OSAL_MutexInit(&ptObj->tMutex);

	eCode = rkFrm_CreateGrp(&ptObj->pBufferGrp, MPP_BUFFER_TYPE_DRM);
    if (ptObj->pBufferGrp == NULL) {
        SysErr("rkFrm_CreateGrp fail. ret: %d\n", eCode);
        goto cleanup;
    }

	for (i = 0; i < DEV_NUM; i++) 
	{
		for(j = 0; j < BannerCacheNum_; j ++)
		{
			MppFrame frame = NULL;
			frame = RKAllocFrame(MPP_FMT_ARGB8888, TYPICAL_WIDTH, TYPICAL_HEIGHT, ptObj->pBufferGrp);
			if(frame != NULL)
			{
				ptObj->bo_ch[i][j] = create_sp_bo_RGB(dev, frame);	
			}
			if (!ptObj->bo_ch[i][j]) {
				SysErr("failed to create plane bo %d.\n", i);
				return STATE_CODE_ALLOCATION_FAILURE;
			}	
		}
	}
    return 0;
cleanup:
	OverlayManager_ModUninit();
	return STATE_CODE_INIT_FAILURE;

}

int OverlayManager_ModUninit() 
{
	int i = 0;
	int j = 0;
	T_Manage *ptObj = OverlayManager();
    if (ptObj) {

		OverlayManager_Stop();
	
		if (NULL != ptObj->hQue_copy)
		 {
			CommQue_Clear(ptObj->hQue_copy);	
			CommQue_Delete(ptObj->hQue_copy);
		 }

		if (NULL != ptObj->hCh0Que)
		 {
			CommQue_Clear(ptObj->hCh0Que);	
			CommQue_Delete(ptObj->hCh0Que);
		 }
		if (NULL != ptObj->hCh1Que)
		 {
			CommQue_Clear(ptObj->hCh1Que);	
			CommQue_Delete(ptObj->hCh1Que);
		 }

		rkFrm_DestroyGrp(&ptObj->pBufferGrp);
		rkComm_ClearAllOfList(&ptObj->tBannerList);
		OSAL_MutexDestroy(&ptObj->tMutex);
		OSAL_MutexDestroy(&ptObj->tBannerMutex);


		for (i = 0; i < DEV_NUM; i++) 
		{
			for(j = 0; j < BannerCacheNum_; j ++)
			{
				free_sp_bo(ptObj->bo_ch[i][j]);
//				RKFreeFrame(ptObj->bo_ch_copy[i][j]);
				ptObj->bo_ch[i][j] = NULL;
//				ptObj->bo_ch_copy[i][j] = NULL;
			}
		}
        free(ptObj);
        ptObj = NULL;
    }
    return 0;
}

E_StateCode OverlayManager_Start() 
{
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_Manage *ptObj = OverlayManager();
	SysLog("OverlayManager_Start start \n");
	if(!ptObj->bState)
	{
		freeALLScrollBannerMem();
		eCode = OverlayManager_FreePlaneInfo();
		ptObj->used_index = 1;
		MODevRegisterGlobalVblank(VBlankCb, ptObj);
		CommQue_Clear(ptObj->hQue_copy);	
		ptObj->bThrdCopyPlayRun = SMP_FALSE;
		TSK_Attrs tTaskAttr2 = DEFAULT_TSK_ATTR;
	    tTaskAttr2.name = "bannerplay";
		ptObj->tCopyPlayTsk = TSK_create(BannerCopy_Cb, &tTaskAttr2, ptObj);
		if (!ptObj->tCopyPlayTsk) 
		{
		     SysErr("Create task for %s failed!\n", "ch1Merge");
			 eCode = STATE_CODE_INIT_FAILURE;
			 goto cleanup;
	 	}
		CommQue_Clear(ptObj->hCh0Que);	
		ptObj->bThrdCh0MergeRun = SMP_FALSE;
		TSK_Attrs tTaskAttr1 = DEFAULT_TSK_ATTR;
	    tTaskAttr1.name = "HDMI0Merge";
		ptObj->tCh0MergeTsk = TSK_create(BannerCh0Merge_Tsk_Cb, &tTaskAttr1, ptObj);
		if (!ptObj->tCh0MergeTsk) 
		{
		     SysErr("Create task for %s failed!\n", "ch0Merge");
			 eCode = STATE_CODE_INIT_FAILURE;
			 goto cleanup;
	 	}	

		CommQue_Clear(ptObj->hCh1Que);	
		ptObj->bThrdCh1MergeRun = SMP_FALSE;
		TSK_Attrs tTaskAttr3 = DEFAULT_TSK_ATTR;
	    tTaskAttr3.name = "HDMI1Merge";
		ptObj->tCh1MergeTsk = TSK_create(BannerCh1Merge_Tsk_Cb, &tTaskAttr3, ptObj);
		if (!ptObj->tCh1MergeTsk) 
		{
		     SysErr("Create task for %s failed!\n", "ch1Merge");
			 eCode = STATE_CODE_INIT_FAILURE;
			 goto cleanup;
	 	}

		ptObj->bState = TRUE;	
		SysLog("OverlayManager_Start done \n");
	}
	return eCode;
cleanup:
	ptObj->bState = TRUE;
	OverlayManager_Stop();
	return eCode;
}

static E_StateCode freeALLScrollBannerMem()
{
	T_Manage *ptObj = OverlayManager();
	int i = 0;
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_BannerNode* ptNode = NULL;
	MppFrame bo = NULL;
	if(ptObj == NULL)
	{
		return STATE_CODE_NO_ERROR;	
	}
	OSAL_MutexLock(&ptObj->tBannerMutex);
	ptNode = (T_BannerNode *)StdListGetHeadNode(&ptObj->tBannerList);
	while (NULL != ptNode)
	{
		OSAL_MutexLock(&ptNode->tCtrlLock);
		for(i = 0; i < ptNode->tCanvas.count; i++)
		{
			bo = ptNode->tCanvas.tempBo[i];
			if(bo != NULL)
			{
				RKFreeFrame(bo);
			}
		}
		if (NULL != ptNode->tCanvas.hImgQue)
		{
			ShareMemQue_Delete(ptNode->tCanvas.hImgQue);
			ptNode->tCanvas.hImgQue = NULL;
		}
		memset(&ptNode->tCanvas, 0x00, sizeof(T_BannerCanvas));
		OSAL_MutexUnlock(&ptNode->tCtrlLock);
		ptNode = (T_BannerNode *)StdListGetNextNode((PT_StdNodeDef)ptNode);
	}
	OSAL_MutexUnlock(&ptObj->tBannerMutex);
	return eCode;
}

E_StateCode OverlayManager_Stop() 
{

	SysLog("OverlayManager_Stop start \n");
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_Manage *ptObj = OverlayManager();	
	ptObj->bState = FALSE;

	MODevUnRegisterGlobalVblank(VBlankCb, ptObj);
	if (NULL != ptObj->tCh0MergeTsk)
	{
		ptObj->bThrdCh0MergeRun = SMP_FALSE;
		CommQue_Clear(ptObj->hCh0Que); 
		TSK_delete(ptObj->tCh0MergeTsk);
		ptObj->tCh0MergeTsk = NULL;	
	}
	if (NULL != ptObj->tCh1MergeTsk)
	{
		ptObj->bThrdCh1MergeRun = SMP_FALSE;
		CommQue_Clear(ptObj->hCh1Que);	
		TSK_delete(ptObj->tCh1MergeTsk);
		ptObj->tCh1MergeTsk = NULL;	
	}

	if (NULL != ptObj->tCopyPlayTsk)
	{
		ptObj->bThrdCopyPlayRun = SMP_FALSE;
		CommQue_Clear(ptObj->hQue_copy);	
		TSK_delete(ptObj->tCopyPlayTsk);
		ptObj->tCopyPlayTsk = NULL;	
	}
	freeALLScrollBannerMem();
	eCode = OverlayManager_FreePlaneInfo();

	SysLog("OverlayManager_Stop done \n");
    return eCode;
}

static E_StateCode reportBanner(T_WindowBufferReq *tSetup, UINT32 uiCurTime, UINT32 uiSrcTime, UINT32 speed, UINT32 fps)
{
	E_StateCode eCode = STATE_CODE_NO_ERROR;
//	T_Manage *ptObj = OverlayManager();	
//	int fps = rkVo_GetFPS(ptObj->tPlane0.VoId);

	T_WindowBuffer		tBuffer;
	T_WindowBuffer		*ptBuffer = &tBuffer;
	memset(ptBuffer, 0x00, sizeof(T_WindowBuffer));
	ptBuffer->uiChId = tSetup->uiChId;
	ptBuffer->uiPlayId = tSetup->uiPlayId;
	ptBuffer->uiResponseTime = tSetup->uiResponseTime;
	ptBuffer->eSyncType = SYNC_TYPE_AUTO;
	ptBuffer->uiWinType = WIN_TYPE_BANNER;

	ptBuffer->uiSrcFps = fps;
	ptBuffer->uiDstFps = fps;
	ptBuffer->eSrcTimeType = TIME_TYPE_SYSTEM;
	ptBuffer->eDstTimeType = TIME_TYPE_SYSTEM;
	ptBuffer->uiSrcCnt = 4;
	ptBuffer->uiCurrentDstTime = uiCurTime;

	ptBuffer->uiMaxJitter = 50;
	ptBuffer->auiSrcTimes[0] = uiSrcTime;
	ptBuffer->auiSrcTimes[1] = uiSrcTime +  (1000/fps) ;
	ptBuffer->auiSrcTimes[2] = ptBuffer->auiSrcTimes[1] +  (1000/fps);
	ptBuffer->auiSrcTimes[3] = ptBuffer->auiSrcTimes[2] +  (1000/fps);
	eCode = MOJsonRpcResponseWindowBuffer(ptBuffer);
	if(MODevDebuglv == 2 )
	{
		SysLog("report ptBuffer->uiChId = %d\n", tSetup->uiChId);
		SysLog("report ptBuffer->uiPlayId = %d\n", tSetup->uiPlayId);
		SysLog("report ptBuffer->uiResponseTime = %u\n", tSetup->uiResponseTime);
		SysLog("report ptBuffer->uiCurrentDstTime = %u\n", uiCurTime);
		SysLog("report ptBuffer->auiSrcTime = %u\n", uiSrcTime);
	}
	return eCode;
}

//static E_StateCode repairBanner(T_WindowCommonCtrl *tSetup)
//{
//	E_StateCode eCode = STATE_CODE_NO_ERROR;
//	return eCode;
//}
//
//static E_StateCode startBanner(T_DisplayBanner *tSetup)
//{
//	E_StateCode eCode = STATE_CODE_NO_ERROR;
//	T_StbpResult *ptResult = NULL;
//	HANDLE				hUser = NULL;
//	String256				strTopic = {0,};
//	String256 strConfig = {0,};
//
//	
//	hUser = GetUserClientHandle();
//	if (NULL == hUser)
//	{
//		SysErr("The user client handle is not create.\n");
//		return STATE_CODE_OBJECT_NOT_EXIST;
//	}
//
//	snprintf(strTopic, sizeof(strTopic), "$request.start.0.$l.$i.banner.v2.mo.ch.%d.idx.%d", tSetup->uiChId, tSetup->uiPlayId);
//	snprintf(strConfig, sizeof(strConfig), "[]");
//
//	SysLog("topic is %s\n", strTopic);
//
//	eCode = StbpClientSendMsgAndParseResult(hUser, strTopic, strConfig, 1000, &ptResult);
//	if(!STATE_OK(eCode))
//	{
//		return eCode;
//	}
//	
//	if(NULL == ptResult)
//	{
//		return STATE_CODE_UNDEFINED_ERROR;
//	}
//
//	if(STATE_CODE_NO_ERROR != ptResult->iCode)
//	{
//		return ptResult->iCode;
//	}
//
//	if(NULL == ptResult->pcData)
//	{
//		goto cleanup;
//	}
//
//cleanup:
//	if(NULL != ptResult)
//	{
//		StbpClientFreeResult(ptResult);
//	}
//
//	return eCode;
//}
//
//static E_StateCode stopBanner(T_DisplayBanner *tSetup)
//{
//	E_StateCode eCode = STATE_CODE_NO_ERROR;
//	T_StbpResult *ptResult = NULL;
//	HANDLE				hUser = NULL;
//	String256				strTopic = {0,};
//	String256 strConfig = {0,};
//	
//	hUser = GetUserClientHandle();
//	if (NULL == hUser)
//	{
//		SysErr("The user client handle is not create.\n");
//		return STATE_CODE_OBJECT_NOT_EXIST;
//	}
//	
//	snprintf(strTopic, sizeof(strTopic), "$request.stop.0.$l.$i.banner.v2.mo.ch.%d.idx.%d", tSetup->uiChId, tSetup->uiPlayId);
//	snprintf(strConfig, sizeof(strConfig), "[]");
//
//	eCode = StbpClientSendMsgAndParseResult(hUser, strTopic, strConfig, 1000, &ptResult);
//	if(!STATE_OK(eCode))
//	{
//		return eCode;
//	}
//	
//	if(NULL == ptResult)
//	{
//		return STATE_CODE_UNDEFINED_ERROR;
//	}
//
//	if(STATE_CODE_NO_ERROR != ptResult->iCode)
//	{
//		return ptResult->iCode;
//	}
//
//	if(NULL == ptResult->pcData)
//	{
//		goto cleanup;
//	}
//
//cleanup:
//	if(NULL != ptResult)
//	{
//		StbpClientFreeResult(ptResult);
//	}
//	return eCode;
//}

E_StateCode OverlayManagerAddPlayInfo(T_OverlayPlayInfo* playinfo)
{
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_Manage *ptObj = OverlayManager();
	if(ptObj == NULL)
	{
		return STATE_CODE_OBJECT_NOT_EXIST;	
	}
	OSAL_MutexLock(&ptObj->tMutex);
	ptObj->used_index = playinfo->index;
	ptObj->tPlane[0].chId = 	playinfo->dev0_chid;
	ptObj->tPlane[1].chId = 	playinfo->dev1_chid;
	ptObj->tPlane[0].dirty_rect = playinfo->dirty_rect[0];
	ptObj->tPlane[1].dirty_rect = playinfo->dirty_rect[1];
	OSAL_MutexUnlock(&ptObj->tMutex);
	return eCode;
}

static T_BannerNode* OverlayFindBannerNode(	UINT32	uiChId, UINT32	uiPlayId)
{
	T_Manage *ptObj = OverlayManager();	
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL)
	{
		return NULL;	
	}
	OSAL_MutexLock(&ptObj->tBannerMutex);
	ptNode = (T_BannerNode *)StdListGetHeadNode(&ptObj->tBannerList);
	while (NULL != ptNode)
	{
		if ((ptNode->tBanner.uiChId == uiChId) && (ptNode->tBanner.uiPlayId == uiPlayId))
		{
			break;
		}
		ptNode = (T_BannerNode *)StdListGetNextNode((PT_StdNodeDef)ptNode);
	}
	OSAL_MutexUnlock(&ptObj->tBannerMutex);
	return ptNode;
}

static T_BannerNode* OverlayFindBannerNode_ForCh(	UINT32	uiChId)
{
	T_Manage *ptObj = OverlayManager();	
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL )
	{
		return NULL;	
	}
	OSAL_MutexLock(&ptObj->tBannerMutex);
	ptNode = (T_BannerNode *)StdListGetHeadNode(&ptObj->tBannerList);
	while (NULL != ptNode)
	{
		if (ptNode->tBanner.uiChId == uiChId)
		{
			break;
		}
		ptNode = (T_BannerNode *)StdListGetNextNode((PT_StdNodeDef)ptNode);
	}
	OSAL_MutexUnlock(&ptObj->tBannerMutex);
	return ptNode;
}

static E_StateCode OverlayAddBannerNode(T_DisplayBanner *tSetup)
{
	T_Manage *ptObj = OverlayManager();	
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL)
	{
		return STATE_CODE_INVALID_PARAM;	
	}
	OSAL_MutexLock(&ptObj->tBannerMutex);
	ptNode = (T_BannerNode *)malloc(sizeof(T_BannerNode));
	if (NULL == ptNode)
	{
		OSAL_MutexUnlock(&ptObj->tBannerMutex);
		return STATE_CODE_ALLOCATION_FAILURE;
	}

	memset(ptNode, 0x0, sizeof(T_BannerNode));
	OSAL_MutexInit(&ptNode->tCtrlLock);
	ptNode->tBanner = *tSetup;
	if(ptNode->tBanner.bannerText.scrollSpeed == 0)
	{
		ptNode->Bstop_scroll = TRUE;
	}
	else
	{
		ptNode->Bstop_scroll = FALSE;	
	}
	ptNode->tCanvas.count = 0;
	StdListPushBack(&ptObj->tBannerList, (PT_StdNodeDef)ptNode);
	OSAL_MutexUnlock(&ptObj->tBannerMutex);
	return eCode;
}

static E_StateCode OverlayDelBannerNode(T_BannerNode *ptNode)
{
	T_Manage *ptObj = OverlayManager();
	int i = 0;
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	MppFrame bo = NULL;
	if(ptObj == NULL)
	{
		return STATE_CODE_INVALID_PARAM;	
	}
	OSAL_MutexLock(&ptObj->tBannerMutex);
	StdListRemove(&ptObj->tBannerList, (PT_StdNodeDef)ptNode);
	

//	if(ptNode->tCanvas.tempBo != NULL)
//	{
//		free_sp_bo_share(ptNode->tCanvas.tempBo);	
//		ptNode->tCanvas.tempBo = NULL;
//	}
	OSAL_MutexLock(&ptNode->tCtrlLock);
	for(i = 0; i < ptNode->tCanvas.count; i++)
	{
		bo = ptNode->tCanvas.tempBo[i];
		if(bo != NULL)
		{
			RKFreeFrame(bo);
		}
	}
	OSAL_MutexUnlock(&ptNode->tCtrlLock);
	if (NULL != ptNode->tCanvas.hImgQue)
	{
		ShareMemQue_Delete(ptNode->tCanvas.hImgQue);
	}
	memset(&ptNode->tCanvas, 0x00, sizeof(T_BannerCanvas));
	OSAL_MutexDestroy(&ptNode->tCtrlLock);
	free(ptNode);
	OSAL_MutexUnlock(&ptObj->tBannerMutex);
	return eCode;
}

static BOOL CheckTimeArrvied(UINT32 uiNeedTime, UINT32 uiCurTime)
{
	if ((uiNeedTime < uiCurTime) || (uiNeedTime < uiCurTime + 9))
	{
		return SMP_TRUE;
	}

	return SMP_FALSE;
}

static E_StateCode OverlayReportBannerInfo(UINT32 uiCurTime)
{
	T_Manage *ptObj = OverlayManager(); 
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL)
	{
		return STATE_CODE_OBJECT_NOT_EXIST; 
	}

	T_WindowBufferReq tSetup;
	memset(&tSetup, 0x0, sizeof(T_WindowBufferReq));
	int fps = rkVo_GetFPS(ptObj->tPlane[0].VoId);
	OSAL_MutexLock(&ptObj->tBannerMutex);
	ptNode = (T_BannerNode *)StdListGetHeadNode(&ptObj->tBannerList);
	while (NULL != ptNode)
	{
		OSAL_MutexLock(&ptNode->tCtrlLock);
		if(ptNode->bReport)
		{
			if(CheckTimeArrvied(ptNode->reportTimestamp , uiCurTime))
			{
				tSetup.uiChId = ptNode->tBanner.uiChId;
				tSetup.uiPlayId = ptNode->tBanner.uiPlayId;	
				tSetup.uiResponseTime = ptNode->reportTimestamp;
				reportBanner(&tSetup, uiCurTime, ptNode->progressTimestamp, ptNode->tBanner.bannerText.scrollSpeed, fps);
				ptNode->bReport = FALSE;
			}
		}
		OSAL_MutexUnlock(&ptNode->tCtrlLock);
		ptNode = (T_BannerNode *)StdListGetNextNode((PT_StdNodeDef)ptNode);
	}
	OSAL_MutexUnlock(&ptObj->tBannerMutex);	
	return	eCode;
}

static E_StateCode OverlayRepairBanner(UINT32 uiCurTime)
{
	T_Manage *ptObj = OverlayManager(); 
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL)
	{
		return STATE_CODE_OBJECT_NOT_EXIST; 
	}
	int fps = rkVo_GetFPS(ptObj->tPlane[0].VoId);
	OSAL_MutexLock(&ptObj->tBannerMutex);
	ptNode = (T_BannerNode *)StdListGetHeadNode(&ptObj->tBannerList);
	while (NULL != ptNode)
	{
		OSAL_MutexLock(&ptNode->tCtrlLock);
		if(ptNode->bRepair)
		{
			int offset_time = (ptNode->SrcTimestamp - ptNode->progressTimestamp);
			UINT32 temp = ptNode->VOTimestamp - uiCurTime;
			offset_time = offset_time - temp;		
			ptNode->correct_offset = offset_time / (1000/fps) * ptNode->tBanner.bannerText.scrollSpeed; 
			if(MODevDebuglv == 3 )
			{
				SysLog("set ptNode->SrcTimestamp = %d\n", ptNode->SrcTimestamp);
				SysLog("set ptNode->progressTimestamp = %d\n", ptNode->progressTimestamp);
				SysLog("set ptNode->VOTimestamp = %u\n", ptNode->VOTimestamp);
				SysLog("set uiCurTime = %u\n", uiCurTime);
				SysLog("set ptNode->correct_offset = %d\n", ptNode->correct_offset);
			}
			ptNode->bRepair = FALSE;
		}
		OSAL_MutexUnlock(&ptNode->tCtrlLock);
		ptNode = (T_BannerNode *)StdListGetNextNode((PT_StdNodeDef)ptNode);
	}
	OSAL_MutexUnlock(&ptObj->tBannerMutex);
	return	eCode;
}

static E_StateCode OverlayRecordBannerInfo(T_Overlay* playinfo)
{
	T_Manage *ptObj = OverlayManager(); 
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL)
	{
		return STATE_CODE_OBJECT_NOT_EXIST; 
	}
	INT32 i = 0;

	OSAL_MutexLock(&ptObj->tBannerMutex);
	ptNode = (T_BannerNode *)StdListGetHeadNode(&ptObj->tBannerList);
	while (NULL != ptNode)
	{
		i = ptNode->tBanner.uiChId * BANNER_NUM + ptNode->tBanner.uiPlayId;
		ptNode->progressTimestamp = playinfo->uiTimestamp_draw[i];
		ptNode = (T_BannerNode *)StdListGetNextNode((PT_StdNodeDef)ptNode);
	}
	OSAL_MutexUnlock(&ptObj->tBannerMutex);
	return	eCode;
}

/**********************************************************************
* 函数名称：setBanner
* 功能描述：向横幅服务配置
* 输入参数：T_Overlay
* 输出参数：无
* 返 回 值：	 状态码
* 其它说明： 
* 修改日期        版本号     修改人	      修改内容
* -----------------------------------------------
* 2024/12/27	     V1.0	           chengjiahao
***********************************************************************/
static E_StateCode setBanner(T_DisplayBanner *tSetup)
{
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_StbpResult *ptResult = NULL;
	HANDLE				hUser = NULL;
	String256				strTopic = {0,};
	INT8 *pConfig = NULL;
	
	hUser = GetUserClientHandle();
	if (NULL == hUser)
	{
		SysErr("The user client handle is not create.\n");
		return STATE_CODE_OBJECT_NOT_EXIST;
	}

	eCode = MediaMakeListElement("banner", (void *)tSetup, &pConfig);
	if(!STATE_OK(eCode))
	{
		SysErr("MediaParseListElement error, eCode = %d\n", eCode);
		return STATE_CODE_INVALID_PARAM;
	}

	snprintf(strTopic, sizeof(strTopic), "$request.$set.0.$l.$i.banner.v2.mo.ch.%d.idx.%d", tSetup->uiChId, tSetup->uiPlayId);

	eCode = StbpClientSendMsgAndParseResult(hUser, strTopic, pConfig, 1000, &ptResult);
	if(!STATE_OK(eCode))
	{
		return eCode;
	}
	
	if(NULL == ptResult)
	{
		return STATE_CODE_UNDEFINED_ERROR;
	}

	if(STATE_CODE_NO_ERROR != ptResult->iCode)
	{
		return ptResult->iCode;
	}

	if(NULL != ptResult)
	{
		StbpClientFreeResult(ptResult);
	}

	return eCode;
}

static E_StateCode clearChALLBanner(UINT32 uiChId)
{
	T_Manage *ptObj = OverlayManager();	
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL)
	{
		return STATE_CODE_OBJECT_NOT_EXIST;	
	}

	ptNode = OverlayFindBannerNode_ForCh(uiChId);
	while (NULL != ptNode)
	{
		ptNode->tBanner.bannerBg.enable = FALSE;
		ptNode->tBanner.bannerText.enable = FALSE;
		if(ptObj->bState)
		{
			eCode = setBanner(&ptNode->tBanner);
		}
		eCode = OverlayDelBannerNode(ptNode);
		ptNode = OverlayFindBannerNode_ForCh(uiChId);

	}		
	return 	eCode;
}

E_StateCode MOChCreateBanner(T_DisplayBanner *tSetup)
{
	T_Manage *ptObj = OverlayManager();	
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL)
	{
		return STATE_CODE_OBJECT_NOT_EXIST;	
	}

	ptNode = OverlayFindBannerNode(tSetup->uiChId, tSetup->uiPlayId);

	if(ptNode != NULL)
	{
		ptNode->tBanner = *tSetup;
		setBanner(tSetup);
	}
	else
	{
		eCode = OverlayAddBannerNode(tSetup);
		if(STATE_OK(eCode))
		{
			if(ptObj->bState)
			{
				setBanner(tSetup);
			}
		}
	}
	ptNode = OverlayFindBannerNode(tSetup->uiChId, tSetup->uiPlayId);
	OSAL_MutexLock(&ptNode->tCtrlLock);
	float changeX = 0.0;
	changeX = (float)TYPICAL_WIDTH / tSetup->Max_rect.uiHor;
	if(ptNode->tBanner.bannerText.scrollSpeed > 0)
	{
		ptNode->tBanner.bannerText.scrollSpeed = (ptNode->tBanner.bannerText.scrollSpeed /10)+1;	
	}
	ptNode->offset = ptNode->tBanner.generateRect.left * changeX;
	OSAL_MutexUnlock(&ptNode->tCtrlLock);
	return 	eCode;
}

E_StateCode MOChDeleteBanner(T_WindowCommonCtrl *tSetup)
{
	T_Manage *ptObj = OverlayManager(); 
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL)
	{
		return STATE_CODE_OBJECT_NOT_EXIST; 
	}

	ptNode = OverlayFindBannerNode(tSetup->uiChId, tSetup->uiPlayId);

	if(ptNode == NULL)
	{
		return STATE_CODE_INVALID_PARAM;		
	}
	else
	{
		ptNode->tBanner.bannerBg.enable = FALSE;
		ptNode->tBanner.bannerText.enable = FALSE;
		ptNode->Bstop_scroll = TRUE;
		if(ptObj->bState)
		{
			eCode = setBanner(&ptNode->tBanner);
		}
		if(STATE_OK(eCode))
		{
			eCode = OverlayDelBannerNode(ptNode);
		}
	}
	return	eCode;
}

E_StateCode MOChUpdateBanner(T_DisplayBanner *tSetup)
{
	T_Manage *ptObj = OverlayManager(); 
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL)
	{
		return STATE_CODE_OBJECT_NOT_EXIST; 
	}

	ptNode = OverlayFindBannerNode(tSetup->uiChId, tSetup->uiPlayId);

	if(ptNode == NULL)
	{
		return MOChCreateBanner(tSetup);
	}

	if(0 == memcmp(tSetup, &ptNode->tBanner, sizeof(T_DisplayBanner)))
	{
		return STATE_CODE_OBJECT_EXISTED;
	}

	ptNode->tBanner = *tSetup;
	if(ptObj->bState)
	{
		eCode = setBanner(tSetup);
	}
	if(!STATE_OK(eCode))
	{
		SysErr("setBanner failed\n");
	}

	OSAL_MutexLock(&ptNode->tCtrlLock);
	if(ptNode->tBanner.bannerText.scrollSpeed > 0)
	{
		ptNode->tBanner.bannerText.scrollSpeed = (ptNode->tBanner.bannerText.scrollSpeed /10)+1;	
	}
	float changeX = 0.0;
	changeX = (float)TYPICAL_WIDTH / tSetup->Max_rect.uiHor;
	ptNode->offset = ptNode->tBanner.generateRect.left * changeX;
	OSAL_MutexUnlock(&ptNode->tCtrlLock);
	return	STATE_CODE_NO_ERROR;
}

E_StateCode MOChStartBanner(T_WindowCommonCtrl *tSetup)
{
	T_Manage *ptObj = OverlayManager(); 
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL)
	{
		return STATE_CODE_OBJECT_NOT_EXIST; 
	}

	ptNode = OverlayFindBannerNode(tSetup->uiChId, tSetup->uiPlayId);

	if(ptNode == NULL)
	{
		return STATE_CODE_INVALID_PARAM;
	}

	if(ptNode->tBanner.bannerText.scrollSpeed > 0 && ptNode->Bstop_scroll)
	{
		OSAL_MutexLock(&ptNode->tCtrlLock);
//		ptNode->tBanner.bannerText.scrollSpeed = (ptNode->tBanner.bannerText.scrollSpeed /10)+1;
		ptNode->Bstop_scroll = FALSE;
		float changeX = 0.0;
		changeX = (float)TYPICAL_WIDTH / ptNode->tBanner.Max_rect.uiHor;
		ptNode->offset = ptNode->tBanner.generateRect.left * changeX;
		OSAL_MutexUnlock(&ptNode->tCtrlLock);
	}
	return	eCode;
}

E_StateCode MOChStopBanner(T_WindowCommonCtrl *tSetup)
{
	T_Manage *ptObj = OverlayManager(); 
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL)
	{
		return STATE_CODE_OBJECT_NOT_EXIST; 
	}

	ptNode = OverlayFindBannerNode(tSetup->uiChId, tSetup->uiPlayId);

	if(ptNode == NULL)
	{
		return STATE_CODE_INVALID_PARAM;		
	}

	if(ptNode->tBanner.bannerText.scrollSpeed > 0 && !ptNode->Bstop_scroll)
	{
		OSAL_MutexLock(&ptNode->tCtrlLock);
		ptNode->Bstop_scroll = TRUE;
		OSAL_MutexUnlock(&ptNode->tCtrlLock);
	}
	return	eCode;
}

E_StateCode MOChGetBannerInfo(T_WindowBufferReq *tSetup)
{
	T_Manage *ptObj = OverlayManager(); 
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL )
	{
		return STATE_CODE_OBJECT_NOT_EXIST; 
	}

	ptNode = OverlayFindBannerNode(tSetup->uiChId, tSetup->uiPlayId);

	if(ptNode == NULL)
	{
		return STATE_CODE_INVALID_PARAM;		
	}
	OSAL_MutexLock(&ptNode->tCtrlLock);
	ptNode->bReport = TRUE; 
	ptNode->reportTimestamp = tSetup->uiResponseTime;
	OSAL_MutexUnlock(&ptNode->tCtrlLock);
	return	eCode;
}

E_StateCode MOChSetBannerInfo(T_WindowPlayCtrl *tSetup)
{
	T_Manage *ptObj = OverlayManager(); 
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL)
	{
		return STATE_CODE_OBJECT_NOT_EXIST; 
	}

	if(tSetup->uiWinType != WIN_TYPE_BANNER)
	{
		return STATE_CODE_INVALID_PARAM;
	}

	ptNode = OverlayFindBannerNode(tSetup->uiChId, tSetup->uiPlayId);

	if(ptNode == NULL)
	{
		return STATE_CODE_INVALID_PARAM;		
	}
	OSAL_MutexLock(&ptNode->tCtrlLock);
	ptNode->bRepair = TRUE; 
	ptNode->VOTimestamp = tSetup->uiDstTime;
	ptNode->SrcTimestamp = tSetup->uiSrcTime;

//	if(MODevDebuglv == 3 )
//	{
////		SysLog("set ptBuffer->uiChId = %d\n", tSetup->uiChId);
////		SysLog("set ptBuffer->uiPlayId = %d\n", tSetup->uiPlayId);
////		SysLog("set ptBuffer->VOTimestamp = %u\n", tSetup->uiDstTime);
//		SysLog("set ptBuffer->SrcTimestamp = %u\n", tSetup->uiSrcTime);
//	}
	OSAL_MutexUnlock(&ptNode->tCtrlLock);
	return	eCode;
}

E_StateCode MOChFreezeBnrChannel(T_WindowCommonCtrl *tSetup)
{
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	return	eCode;
}

E_StateCode MOChUnFreezeBnrChannel(T_WindowCommonCtrl *tSetup)
{
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	return	eCode;
}

E_StateCode MOChOpenMultiBanner(T_DisplayBanner *tSetup, UINT32 num, UINT32 uiChId)
{
	T_Manage *ptObj = OverlayManager(); 
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL)
	{
		return STATE_CODE_OBJECT_NOT_EXIST; 
	}
	int i = 0;
	if(num > 0)
	{
		for (i = 0; i < num; i++)
		{
			eCode = MOChUpdateBanner(tSetup);
			if(!STATE_OK(eCode))
			{
				ptNode = OverlayFindBannerNode(tSetup->uiChId, tSetup->uiPlayId);
				OSAL_MutexLock(&ptNode->tCtrlLock);
				if(ptNode->Bstop_scroll)
				{
					ptNode->Bstop_scroll = FALSE;	
				}
				OSAL_MutexUnlock(&ptNode->tCtrlLock);	
			}
			tSetup++;
		}
	}
	else
	{
		eCode = clearChALLBanner(uiChId);	
	}

	return	STATE_CODE_NO_ERROR;
}

/**********************************************************************
* 函数名称：OverlayManagerPlay
* 功能描述：图像输出,时戳记录,纠正和上报
* 输入参数：T_Overlay
* 输出参数：无
* 返 回 值：	 状态码
* 其它说明： 
* 修改日期        版本号     修改人	      修改内容
* -----------------------------------------------
* 2024/12/27	     V1.0	           chengjiahao
***********************************************************************/
static E_StateCode OverlayManagerPlay(T_Overlay* playinfo)
{
	T_Manage *ptObj = OverlayManager(); 
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	int i = 0;
	
	if(ptObj == NULL || !ptObj->bState)
	{
		return STATE_CODE_OBJECT_NOT_EXIST; 
	}
	UINT32 uiCurTime = 0;
	
	struct sp_bo *pSpBo = NULL;

	for(i = 0; i <= HDMIOUT_2; i++)
	{
		pSpBo = NULL;
		pSpBo = ptObj->tPlane[i].bo[playinfo->index];
		eCode = MODevPlayOverlayPlane_bySpBo(pSpBo, i);
		if(!STATE_OK(eCode))
		{
			SysWarn("MODevPlayOverlayPlane_byIndex failed\n");
		}
	}

	eCode = OverlayRecordBannerInfo(playinfo);
	if(!STATE_OK(eCode))
	{
		SysErr("OverlayRecordBannerInfo failed\n");
	}
	uiCurTime = OSAL_GetCurTimeInMsec();
	eCode = OverlayReportBannerInfo(uiCurTime);
	if(!STATE_OK(eCode))
	{
		SysErr("OverlayReportBannerInfo failed\n");
	}
	
	eCode = OverlayRepairBanner(uiCurTime);
	if(!STATE_OK(eCode))
	{
		SysErr("OverlayRepairBanner failed\n");
	}	
	return	eCode;
}

E_StateCode OverlayManager_SetPlaneInfo(UINT32 pPlaneIndex, UINT32 voId, T_OverLayFB* fbinfo)
{
	T_Manage *ptObj = OverlayManager(); 
	struct sp_dev *dev = NULL;
	MppFrame frame = NULL;
	
	dev = rkVo_GetSpDev(0);
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	int i=  0;
	if(ptObj == NULL)
	{
		return STATE_CODE_OBJECT_NOT_EXIST; 
	}

//	if(!ptObj->bState)
//	{
//		return STATE_CODE_OBJECT_LOCKED; 
//	}
	
	OSAL_MutexLock(&ptObj->tMutex);
	ptObj->tPlane[voId].VoId = voId;
	ptObj->tPlane[voId].plane = pPlaneIndex;
	ptObj->tPlane[voId].dirty_rect.x = 0;
	ptObj->tPlane[voId].dirty_rect.y = 0;
	ptObj->tPlane[voId].dirty_rect.w = TYPICAL_WIDTH;
	ptObj->tPlane[voId].dirty_rect.h = TYPICAL_HEIGHT;
	ptObj->tPlane[voId].enable = TRUE;
	for(i = 0; i < BannerCacheNum; i ++)
	{
		if(ptObj->tPlane[voId].bo[i] == NULL)
		{
			frame = NULL;
			frame = RKAllocFrame(MPP_FMT_ARGB8888, TYPICAL_WIDTH, TYPICAL_HEIGHT, ptObj->pBufferGrp);
			if(frame != NULL)
			{
				ptObj->tPlane[voId].bo[i] = create_sp_bo_show(dev, frame);	
			}
			if (!ptObj->tPlane[voId].bo[i]) {
				SysErr("failed to create plane bo %d.\n", i);
			}		
		}

	}	

	for (i = 0; i < OverlayCacheNum; i++)
	{
		if(ptObj->tPlane[voId].bo_share[i] != NULL)
		{
			free_sp_bo(ptObj->tPlane[voId].bo_share[i]);
			ptObj->tPlane[voId].bo_share[i] = NULL;	
		}
    }
	

	for (i = 0; i < OverlayCacheNum; i++)
	{
		frame = NULL;
		frame = RKAllocFrame(MPP_FMT_ARGB8888, fbinfo->width, fbinfo->height, ptObj->pBufferGrp);
		if(frame != NULL)
		{
			ptObj->tPlane[voId].bo_share[i] = create_sp_bo_share(dev, frame);	
		}
        if (!ptObj->tPlane[voId].bo_share[i])
		{
            SysErr("failed to create plane bo %d.\n", i);
        }
    }

	fbinfo->flags = ptObj->tPlane[voId].bo_share[0]->flags;
	fbinfo->depth = ptObj->tPlane[voId].bo_share[0]->depth;
	fbinfo->pitch = ptObj->tPlane[voId].bo_share[0]->pitch;
	fbinfo->size = ptObj->tPlane[voId].bo_share[0]->size;
	for(i = 0; i < OverlayCacheNum; i++)
	{
		fbinfo->fbinfo[i].fb_id = ptObj->tPlane[voId].bo_share[i]->fb_id;
		fbinfo->fbinfo[i].handle = ptObj->tPlane[voId].bo_share[i]->handle;
		fbinfo->fbinfo[i].name = ptObj->tPlane[voId].bo_share[i]->name;
		fbinfo->fbinfo[i].index = i;
	}
	OSAL_MutexUnlock(&ptObj->tMutex);
	return eCode;
}

E_StateCode OverlayManager_FreePlaneInfo()
{
	T_Manage *ptObj = OverlayManager(); 

	E_StateCode eCode = STATE_CODE_NO_ERROR;
	int i=  0, j = 0;
	if(ptObj == NULL)
	{
		return STATE_CODE_OBJECT_NOT_EXIST; 
	}
	OSAL_MutexLock(&ptObj->tMutex);

	for(j = 0; j < DEV_NUM; j++)
	{
		if(ptObj->tPlane[j].enable)
		{
			ptObj->tPlane[j].dirty_rect.x = 0;
			ptObj->tPlane[j].dirty_rect.y = 0;
			ptObj->tPlane[j].dirty_rect.w = 0;
			ptObj->tPlane[j].dirty_rect.h = 0;
			ptObj->tPlane[j].enable = FALSE;
			for(i = 0; i < BannerCacheNum; i ++)
			{
				free_sp_bo(ptObj->tPlane[j].bo[i]);
				ptObj->tPlane[j].bo[i] = NULL;
			}	
			
			for (i = 0; i < OverlayCacheNum; i++)
			{

		        free_sp_bo(ptObj->tPlane[j].bo_share[i]);
				ptObj->tPlane[j].bo_share[i] = NULL;
		    }
		}	

	}

	OSAL_MutexUnlock(&ptObj->tMutex);
	return eCode;
}

E_StateCode OverlayManagerCreatScrollBanner(T_OverlayScroll_Info * tSetup)
{
	T_Manage *ptObj = OverlayManager();
	UINT32				uiFrameSize = 0;
	UINT32 i = 0;
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL || tSetup == NULL  || !ptObj->bState)
	{
		return STATE_CODE_OBJECT_NOT_EXIST; 
	}
	MppFrame bo = NULL;

//	struct sp_dev *dev = NULL;
//	dev = rkVo_GetSpDev(0);
	ptNode = OverlayFindBannerNode(tSetup->uiChId, tSetup->uiPlayId);
	
	if(ptNode != NULL)
	{
		OSAL_MutexLock(&ptNode->tCtrlLock);
		
		for(i = 0; i < ptNode->tCanvas.count; i++)
		{
			bo = ptNode->tCanvas.tempBo[i];
			if(bo != NULL)
			{
				RKFreeFrame(bo);
			}
		}
		
		if (NULL != ptNode->tCanvas.hImgQue)
		{
			ShareMemQue_Delete(ptNode->tCanvas.hImgQue);
		}
		memset(&ptNode->tCanvas, 0x00, sizeof(T_BannerCanvas));
		if(tSetup->width <= RGA_MAX_width)
		{
			ptNode->tCanvas.count = 1;
			ptNode->tCanvas.tempBo[0] = RKAllocFrame(MPP_FMT_ARGB8888, tSetup->width, tSetup->height, ptObj->pBufferGrp);
			RKFillFrame(ptNode->tCanvas.tempBo[0], NULL, 0x00000000);
			SysLog("tempBo width is %d\n", tSetup->width);	
		}
		else
		{
			ptNode->tCanvas.count = (tSetup->width /RGA_MAX_width) + 1;
			for(i = 0; i < ptNode->tCanvas.count ; i++)
			{
				ptNode->tCanvas.tempBo[i] = RKAllocFrame(MPP_FMT_ARGB8888, RGA_MAX_width, tSetup->height, ptObj->pBufferGrp);
				RKFillFrame(ptNode->tCanvas.tempBo[i], NULL, 0x00000000);
				SysLog("tempBo width is %d\n", RGA_MAX_width);
			}
		}
		
		uiFrameSize = tSetup->width * tSetup->height * 4;
		SafeSprintf(tSetup->fbinfo.strId, sizeof(tSetup->fbinfo.strId), NULL, "scroll%d_%d", tSetup->uiChId, tSetup->uiPlayId);
		ptNode->tCanvas.hImgQue = ShareMemQue_Create(2, uiFrameSize, tSetup->fbinfo.strId);

		ptNode->tCanvas.width = tSetup->width;
		ptNode->tCanvas.height = tSetup->height;
//		SysLog("strid is %s\n", tSetup->fbinfo.strId);
//		tSetup->size = ptNode->tCanvas.tempBo->size;
//		tSetup->fbinfo.fb_id = ptNode->tCanvas.tempBo->fb_id;
//		tSetup->fbinfo.handle = ptNode->tCanvas.tempBo->handle;
//		tSetup->fbinfo.name = ptNode->tCanvas.tempBo->name;
		OSAL_MutexUnlock(&ptNode->tCtrlLock);
	}
	else
	{
		eCode = STATE_CODE_OBJECT_NOT_EXIST;
	}
	return	eCode;
}

E_StateCode MOChPrintMultiBannerInfo(UINT32 uiChId)
{
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_Manage *ptObj = OverlayManager(); 
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL  || !ptObj->bState)
	{
		return STATE_CODE_OBJECT_NOT_EXIST; 
	}

	PrintKeyInfo("banner info of channel %d::\n", uiChId);
	OSAL_MutexLock(&ptObj->tBannerMutex);
	ptNode = (T_BannerNode *)StdListGetHeadNode(&ptObj->tBannerList);
	while (NULL != ptNode)
	{
		if (ptNode->tBanner.uiChId == uiChId)
		{
			PrintKeyInfo("----------------------------idx%d-------------------------\n", ptNode->tBanner.uiPlayId);
			PrintKeyInfo("enable=%d\n", ptNode->tBanner.bannerBg.enable);
			PrintKeyInfo("isPureColorBg=%d\n", ptNode->tBanner.bannerBg.isPureColorBg);
			PrintKeyInfo("bgColor=%s\n", ptNode->tBanner.bannerBg.bgColor);
			PrintKeyInfo("opacity=%d\n", ptNode->tBanner.bannerBg.opacity);
			PrintKeyInfo("imgFile=%s\n\n", ptNode->tBanner.bannerBg.imgFile);
			
			PrintKeyInfo("enable=%d\n", ptNode->tBanner.bannerText.enable);
			PrintKeyInfo("fontSize=%d\n", ptNode->tBanner.bannerText.fontSize);
			PrintKeyInfo("content=%s\n", ptNode->tBanner.bannerText.content);
			PrintKeyInfo("uiColor=%s\n", ptNode->tBanner.bannerText.foreColor);
			PrintKeyInfo("speed=%d\n", ptNode->tBanner.bannerText.scrollSpeed);
			
			PrintKeyInfo("generateRect.left=%d\n", ptNode->tBanner.generateRect.left);
			PrintKeyInfo("generateRect.top=%d\n", ptNode->tBanner.generateRect.top);
			PrintKeyInfo("generateRect.right=%d\n", ptNode->tBanner.generateRect.right);
			PrintKeyInfo("generateRect.bottom=%d\n", ptNode->tBanner.generateRect.bottom);
			
			PrintKeyInfo("showRect.left=%d\n", ptNode->tBanner.showRect.left);
			PrintKeyInfo("showRect.top=%d\n", ptNode->tBanner.showRect.top);
			PrintKeyInfo("showRect.right=%d\n", ptNode->tBanner.showRect.right);
			PrintKeyInfo("showRect.bottom=%d\n", ptNode->tBanner.showRect.bottom);
			
			PrintKeyInfo("base.h=%d\n", ptNode->tBanner.base.uiHor);
			PrintKeyInfo("base.v=%d\n", ptNode->tBanner.base.uiVer);
			PrintKeyInfo("Max_base.h=%d\n", ptNode->tBanner.Max_rect.uiHor);
			PrintKeyInfo("Max_base.v=%d\n", ptNode->tBanner.Max_rect.uiVer);
			PrintKeyInfo("canvasState=%d\n", ptNode->tCanvas.bEnable);
			PrintKeyInfo("offset=%d\n", ptNode->offset);
			PrintKeyInfo("stop=%d\n", ptNode->Bstop_scroll);
//			PrintKeyInfo("tempBo=%p\n", ptNode->tCanvas.tempBo);
			
			PrintKeyInfo("------------------------End of Win%d----------------------\n\n", ptNode->tBanner.uiPlayId);	
		}
		ptNode = (T_BannerNode *)StdListGetNextNode((PT_StdNodeDef)ptNode);
	}
	OSAL_MutexUnlock(&ptObj->tBannerMutex);	
	return eCode;
}

E_StateCode OverlayManagerFleshBanners()
{
	T_Manage *ptObj = OverlayManager(); 
	E_StateCode eCode = STATE_CODE_NO_ERROR;
	T_BannerNode* ptNode = NULL;
	if(ptObj == NULL)
	{
		return STATE_CODE_OBJECT_NOT_EXIST; 
	}

	OSAL_MutexLock(&ptObj->tBannerMutex);
	ptNode = (T_BannerNode *)StdListGetHeadNode(&ptObj->tBannerList);
	while (NULL != ptNode)
	{
		if(ptObj->bState)
		{
			eCode = setBanner(&ptNode->tBanner);
		}
		if(!STATE_OK(eCode))
		{
			SysErr("setBanner failed\n");
		}
		if(ptNode->tBanner.bannerText.scrollSpeed > 0)
		{
			OSAL_MutexLock(&ptNode->tCtrlLock);
			if(ptNode->Bstop_scroll)
			{
				float changeX = 0.0;
				changeX = (float)TYPICAL_WIDTH / ptNode->tBanner.Max_rect.uiHor;
				ptNode->offset = ptNode->tBanner.generateRect.left * changeX;
				ptNode->Bstop_scroll = FALSE;	
			}
			OSAL_MutexUnlock(&ptNode->tCtrlLock);
		}
		ptNode = (T_BannerNode *)StdListGetNextNode((PT_StdNodeDef)ptNode);
	}
	OSAL_MutexUnlock(&ptObj->tBannerMutex);
	return	eCode;
}
