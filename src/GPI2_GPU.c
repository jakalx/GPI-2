/*
Copyright (c) Fraunhofer ITWM - Carsten Lojewski <lojewski@itwm.fhg.de>, 2013-2014

This file is part of GPI-2.

GPI-2 is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License
version 3 as published by the Free Software Foundation.

GPI-2 is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GPI-2. If not, see <http://www.gnu.org/licenses/>.
*/

#include <cuda_runtime_api.h>
#include <cuda.h>
#include <string.h>
#include "GASPI.h"
#include "GPI2.h"
#include "GPI2_IB.h"
#include "GPI2_GPU.h"
#include "GASPI_GPU.h"
#include <pthread.h>

void* cudaThread(void *data); 

static int _gaspi_find_GPU_numa_node(int cudevice)
{

  CUresult cres;
  int domain, bus, dev;
  char path[128];
  FILE *sysfile = NULL;

#ifdef CU_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID
  cres = cuDeviceGetAttribute(&domain, CU_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID, cudevice);
  if (cres != CUDA_SUCCESS)
  {
    //TODO:gaspi_print_error
    errno = ENOSYS;
    return -1;
  }
#else
  domain = 0;
#endif

  cres = cuDeviceGetAttribute(&bus, CU_DEVICE_ATTRIBUTE_PCI_BUS_ID, cudevice);
  if (cres != CUDA_SUCCESS)
  {
    return -1;
  }

  cres = cuDeviceGetAttribute(&dev, CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID, cudevice);
  if (cres != CUDA_SUCCESS)
  {
    return -1;
  }

  sprintf(path, "/sys/bus/pci/devices/%04x:%02x:%02x.0/numa_node", domain, bus, dev);
  sysfile = fopen(path, "r");
  if (!sysfile)
    return -1;

  int numa_node;
  fscanf (sysfile, "%1d", &numa_node);
  fclose(sysfile);

  return numa_node;
}

static int _gaspi_find_GPU_ib_numa_node()
{
  char path[128];
  FILE *sysfile = NULL;

  sprintf(path, "/sys/class/infiniband/%s/device/numa_node",
      ibv_get_device_name(glb_gaspi_ctx_ib.ib_dev));

  sysfile = fopen(path, "r");

  if (!sysfile)
    return -1;

  int numa_node;
  fscanf (sysfile, "%1d", &numa_node);
  fclose(sysfile);

  return numa_node;
}

gaspi_return_t gaspi_init_GPUs()
{
  int device_id = 0;
  int i,j,k;
  int deviceCount;
  int gaspi_devices = 0;
  int ib_numa_node;

  cudaError_t error_id = cudaGetDeviceCount(&deviceCount);
  if (error_id != cudaSuccess)
  {
    gaspi_print_error("cudaGetDeviceCount != cudaSuccess" );
    return GASPI_ERROR; 
  }

  if(deviceCount <= 0)
  {
    gaspi_print_error("no CUDA capable devices found \n");
    return GASPI_ERROR;
  }

  ib_numa_node = _gaspi_find_GPU_ib_numa_node();
  int direct_devices[32];
  struct cudaDeviceProp deviceProp;
  for(device_id =0; device_id<deviceCount; device_id++)
  {
    cudaGetDeviceProperties(&deviceProp, device_id);
    if(deviceProp.major >= 3)
    {
      cudaSetDevice(device_id);
      if(ib_numa_node == _gaspi_find_GPU_numa_node(device_id))
      {
        direct_devices[gaspi_devices] = device_id;
        gaspi_devices++;
      }
    }
  }

  if(gaspi_devices == 0)
  {
    gaspi_print_error("No GPU Direct RDMA capable devices on the right NUMA-socket were found!\n");
    return GASPI_ERROR; 
  }

  glb_gaspi_ctx.gpu_count = gaspi_devices;

  gpus =(gaspi_gpu*)  malloc(sizeof(gaspi_gpu)*glb_gaspi_ctx.gpu_count);

  for(k = 0 ; k < gaspi_devices; k++) 
  {
    cudaSetDevice(direct_devices[k]);

    for( i = 0; i < GASPI_MAX_QP; i++)
    {
      cudaStreamCreate(&gpus[k].streams[i]);
      for( j = 0; j < GASPI_CUDA_EVENTS; j++)
      {
        cudaEventCreateWithFlags(&gpus[k].events[i][j].event, cudaEventDisableTiming);
      }

      cudaStreamCreateWithFlags(&gpus[k].streams[i], cudaStreamNonBlocking);
    }

    gpus[k].device_id = direct_devices[k];
  }

  glb_gaspi_ctx.use_gpus = 1;

  return GASPI_SUCCESS;
}

gaspi_return_t gaspi_number_of_GPUs(gaspi_gpu_num* gpus)
{
  if (!glb_gaspi_init)
  {
    gaspi_print_error("Invalid function before gaspi_proc_init");
    return GASPI_ERROR;
  }

  if (glb_gaspi_ctx.use_gpus == 0)
  {
    gaspi_print_error("GPUs are not initialized");
    return GASPI_ERROR;
  }

  *gpus = glb_gaspi_ctx.gpu_count;

  return GASPI_SUCCESS;
}

gaspi_return_t gaspi_GPU_ids(gaspi_gpu_t* gpuIds)
{
  if (!glb_gaspi_init)
  {
    gaspi_print_error("Invalid function before gaspi_proc_init");
    return GASPI_ERROR;
  }

  if (glb_gaspi_ctx.use_gpus == 0)
  {
    gaspi_print_error("GPUs are not initialized");
    return GASPI_ERROR;
  }

  int i;
  for (i = 0; i < glb_gaspi_ctx.gpu_count; i++)
    gpuIds[i] = gpus[i].device_id;

  return GASPI_SUCCESS;
}


static int _gaspi_event_send(gaspi_cuda_event *event, int queue)
{
  struct ibv_send_wr swr;
  struct ibv_sge slist;
  struct ibv_send_wr *bad_wr; 

  swr.wr.rdma.rkey = glb_gaspi_ctx_ib.rrmd[event->segment_remote][event->rank].rkey;
  swr.sg_list    = &slist;
  swr.num_sge    = 1;
  swr.wr_id      = event->rank;
  swr.opcode     = IBV_WR_RDMA_WRITE;
  swr.send_flags = IBV_SEND_SIGNALED;
  swr.next       = NULL;

  slist.addr = (uintptr_t) (char*)(glb_gaspi_ctx_ib.rrmd[event->segment_local][event->rank].host_ptr+NOTIFY_OFFSET+event->offset_local);

  slist.length = event->size;
  slist.lkey = glb_gaspi_ctx_ib.rrmd[event->segment_local][glb_gaspi_ctx.rank].host_mr->lkey;

  if(glb_gaspi_ctx_ib.rrmd[event->segment_remote][event->rank].cudaDevId >= 0)
    swr.wr.rdma.remote_addr = (glb_gaspi_ctx_ib.rrmd[event->segment_remote][event->rank].addr+event->offset_remote);
  else
    swr.wr.rdma.remote_addr = (glb_gaspi_ctx_ib.rrmd[event->segment_remote][event->rank].addr+NOTIFY_OFFSET+event->offset_remote);

  if(ibv_post_send(glb_gaspi_ctx_ib.qpC[queue][event->rank],&swr,&bad_wr))
  {
    glb_gaspi_ctx.qp_state_vec[queue][event->rank]=1;       
    return GASPI_ERROR; 
  }

  event->ib_use = 1;

  return 0;
}



#pragma weak gaspi_gpu_write = pgaspi_gpu_write 

gaspi_return_t pgaspi_gpu_write(const gaspi_segment_id_t segment_id_local,
    const gaspi_offset_t offset_local,
    const gaspi_rank_t rank,
    const gaspi_segment_id_t segment_id_remote,
    const gaspi_offset_t offset_remote,
    const gaspi_size_t size,
    const gaspi_queue_id_t queue,
    const gaspi_timeout_t timeout_ms)
{
  if(glb_gaspi_ctx_ib.rrmd[segment_id_local][glb_gaspi_ctx.rank].cudaDevId < 0)
    return gaspi_write(segment_id_local, offset_local, rank, segment_id_remote, offset_remote, size, queue, timeout_ms);

  if(size <= GASPI_GPU_DIRECT_MAX)
    return gaspi_write(segment_id_local, offset_local, rank, segment_id_remote, offset_remote, size, queue, timeout_ms);

  if(lock_gaspi_tout (&glb_gaspi_ctx.lockC[queue], timeout_ms))
    return GASPI_TIMEOUT;

  char* host_ptr = (char*)(glb_gaspi_ctx_ib.rrmd[segment_id_local][glb_gaspi_ctx.rank].host_ptr+NOTIFY_OFFSET+offset_local);
  char* device_ptr =(char*)(glb_gaspi_ctx_ib.rrmd[segment_id_local][glb_gaspi_ctx.rank].addr+offset_local);

  int size_left = size;

  int BLOCK_SIZE= GASPI_GPU_BUFFERED;


  gaspi_gpu* agpu =  _gaspi_find_gpu(glb_gaspi_ctx_ib.rrmd[segment_id_local][glb_gaspi_ctx.rank].cudaDevId);
  if(!agpu)
  {
    gaspi_print_error("No GPU found. Maybe forgot to call gaspi_init_GPUs?\n");
    return GASPI_ERROR;
  }

  const gaspi_cycles_t s0 = gaspi_get_cycles ();

  int copy_size = 0;
  int gpu_offset = 0;

  while(size_left > 0)
  {
    int i;
    for(i = 0; i < GASPI_CUDA_EVENTS; i++)
    {
      if(size_left > BLOCK_SIZE)
        copy_size = BLOCK_SIZE;
      else 
        copy_size = size_left;

      if(cudaMemcpyAsync(host_ptr+gpu_offset, device_ptr+gpu_offset, copy_size,cudaMemcpyDeviceToHost,agpu->streams[queue]))
      {
        unlock_gaspi(&glb_gaspi_ctx.lockC[queue]);
        return GASPI_ERROR;
      }

      glb_gaspi_ctx_ib.ne_count_c[queue]++;        

      agpu->events[queue][i].segment_remote = segment_id_remote;
      agpu->events[queue][i].segment_local = segment_id_local;
      agpu->events[queue][i].size = copy_size;
      agpu->events[queue][i].rank = rank;
      agpu->events[queue][i].offset_local = offset_local+gpu_offset;
      agpu->events[queue][i].offset_remote = offset_remote+gpu_offset;
      agpu->events[queue][i].in_use =1;

      cudaError_t err = cudaEventRecord(agpu->events[queue][i].event,agpu->streams[queue]); 
      if(err != cudaSuccess)
      {
        glb_gaspi_ctx.qp_state_vec[queue][rank]=1;
        unlock_gaspi(&glb_gaspi_ctx.lockC[queue]);
        return GASPI_ERROR;
      }

      gpu_offset += copy_size;
      size_left -= copy_size;

      if(size_left == 0)
        break;

      if(agpu->events[queue][i].ib_use)
      {
        struct ibv_wc wc;
        int ne;
        do
        {
          ne = ibv_poll_cq (glb_gaspi_ctx_ib.scqC[queue], 1, &wc);
          glb_gaspi_ctx_ib.ne_count_c[queue] -= ne;
          if (ne == 0)
          {
            const gaspi_cycles_t s1 = gaspi_get_cycles ();
            const gaspi_cycles_t tdelta = s1 - s0;

            const float ms = (float) tdelta * glb_gaspi_ctx.cycles_to_msecs;
            if (ms > timeout_ms)
            {
              unlock_gaspi (&glb_gaspi_ctx.lockC[queue]);
              return GASPI_TIMEOUT;
            }
          } 
        } while(ne==0);
        agpu->events[queue][i].ib_use = 0;
      }
    }

    for(i = 0; i < GASPI_CUDA_EVENTS; i++)
    {
      cudaError_t error;
      if (agpu->events[queue][i].in_use ==1)
      {
        do
        {
          error= cudaEventQuery(agpu->events[queue][i].event );
          if(error==CUDA_SUCCESS)
          {
            _gaspi_event_send(&agpu->events[queue][i],queue);
            agpu->events[queue][i].in_use =0;
          }
          else if(error == cudaErrorNotReady)
          {
            const gaspi_cycles_t s1 = gaspi_get_cycles ();
            const gaspi_cycles_t tdelta = s1 - s0;

            const float ms = (float) tdelta * glb_gaspi_ctx.cycles_to_msecs;
            if (ms > timeout_ms)
            {
              unlock_gaspi (&glb_gaspi_ctx.lockC[queue]);
              return GASPI_TIMEOUT;
            }
          }
          else
          { 
            unlock_gaspi (&glb_gaspi_ctx.lockC[queue]);
            return GASPI_ERROR;
          }
        } while(error != CUDA_SUCCESS);
      }
    } 
  }

  unlock_gaspi (&glb_gaspi_ctx.lockC[queue]);

  return GASPI_SUCCESS; 
}

#pragma weak gaspi_gpu_write_notify = pgaspi_gpu_write_notify
gaspi_return_t pgaspi_gpu_write_notify(const gaspi_segment_id_t segment_id_local,
    const gaspi_offset_t offset_local,
    const gaspi_rank_t rank,
    const gaspi_segment_id_t segment_id_remote,
    const gaspi_offset_t offset_remote,
    const gaspi_size_t size,
    const gaspi_notification_id_t notification_id,
    const gaspi_notification_t notification_value,
    const gaspi_queue_id_t queue,
    const gaspi_timeout_t timeout_ms)
{

  if(glb_gaspi_ctx_ib.rrmd[segment_id_local][glb_gaspi_ctx.rank].cudaDevId < 0)
    return gaspi_write_notify(segment_id_local, offset_local, rank, segment_id_remote, offset_remote, size,notification_id, notification_value, queue, timeout_ms);

  if(size <= GASPI_GPU_DIRECT_MAX)
    return gaspi_write_notify(segment_id_local, offset_local, rank, segment_id_remote, offset_remote, size,notification_id, notification_value, queue, timeout_ms);

  if(lock_gaspi_tout (&glb_gaspi_ctx.lockC[queue], timeout_ms))
    return GASPI_TIMEOUT;

  char *host_ptr = (char*)(glb_gaspi_ctx_ib.rrmd[segment_id_local][glb_gaspi_ctx.rank].host_ptr+NOTIFY_OFFSET+offset_local);
  char* device_ptr =(char*)(glb_gaspi_ctx_ib.rrmd[segment_id_local][glb_gaspi_ctx.rank].addr+offset_local);

  int size_left = size;

  int BLOCK_SIZE= GASPI_GPU_BUFFERED;


  gaspi_gpu* agpu =  _gaspi_find_gpu(glb_gaspi_ctx_ib.rrmd[segment_id_local][glb_gaspi_ctx.rank].cudaDevId);
  if(!agpu)
  {
    gaspi_print_error("No GPU found. Maybe forgot to call gaspi_init_GPUs?\n");
    unlock_gaspi(&glb_gaspi_ctx.lockC[queue]);
    return GASPI_ERROR;
  }

  const gaspi_cycles_t s0 = gaspi_get_cycles ();
  int copy_size = 0;
  int gpu_offset = 0;

  while(size_left > 0)
  {
    int i;
    for(i = 0; i < GASPI_CUDA_EVENTS; i++)
    {
      if(size_left > BLOCK_SIZE)
        copy_size = BLOCK_SIZE;
      else 
        copy_size = size_left;

      if(cudaMemcpyAsync(host_ptr+gpu_offset, device_ptr+gpu_offset, copy_size,cudaMemcpyDeviceToHost,agpu->streams[queue]))
      {
        unlock_gaspi(&glb_gaspi_ctx.lockC[queue]);
        return GASPI_ERROR;
      }

      glb_gaspi_ctx_ib.ne_count_c[queue]++;        

      agpu->events[queue][i].segment_remote = segment_id_remote;
      agpu->events[queue][i].segment_local = segment_id_local;
      agpu->events[queue][i].size = copy_size;
      agpu->events[queue][i].rank = rank;
      agpu->events[queue][i].offset_local = offset_local+gpu_offset;
      agpu->events[queue][i].offset_remote = offset_remote+gpu_offset; 
      agpu->events[queue][i].in_use  = 1; 
      cudaError_t err = cudaEventRecord(agpu->events[queue][i].event,agpu->streams[queue]); 
      if(err!=cudaSuccess)
      {
        unlock_gaspi(&glb_gaspi_ctx.lockC[queue]);
        return GASPI_ERROR;
      }
      /* Thats not beautiful at all, however, else we have a overflow soon in the queue */
      if(agpu->events[queue][i].ib_use)
      {
        struct ibv_wc wc;
        int ne;
        do
        {
          ne = ibv_poll_cq (glb_gaspi_ctx_ib.scqC[queue], 1, &wc);
          glb_gaspi_ctx_ib.ne_count_c[queue] -= ne;
          if (ne == 0)
          {
            const gaspi_cycles_t s1 = gaspi_get_cycles ();
            const gaspi_cycles_t tdelta = s1 - s0;

            const float ms = (float) tdelta * glb_gaspi_ctx.cycles_to_msecs;
            if (ms > timeout_ms)
            {
              unlock_gaspi (&glb_gaspi_ctx.lockC[queue]);
              return GASPI_TIMEOUT;
            }
          } 

        } while(ne == 0);
        agpu->events[queue][i].ib_use = 0;
      }

      gpu_offset += copy_size;
      size_left -= copy_size;
      if(size_left == 0)
        break; 
    } 
    for(i = 0; i < GASPI_CUDA_EVENTS; i++)
    {
      cudaError_t error;
      if (agpu->events[queue][i].in_use == 1)
      {
        do
        {
          error= cudaEventQuery(agpu->events[queue][i].event );
          if(error==CUDA_SUCCESS)
          {
            _gaspi_event_send(&agpu->events[queue][i],queue);
            agpu->events[queue][i].in_use  = 0; 
          }
          else if(error==cudaErrorNotReady)
          {
            const gaspi_cycles_t s1 = gaspi_get_cycles ();
            const gaspi_cycles_t tdelta = s1 - s0;

            const float ms = (float) tdelta * glb_gaspi_ctx.cycles_to_msecs;
            if (ms > timeout_ms)
            {
              unlock_gaspi (&glb_gaspi_ctx.lockC[queue]);
              return GASPI_TIMEOUT;
            }
          }
          else
          { 
            unlock_gaspi (&glb_gaspi_ctx.lockC[queue]);
            return GASPI_ERROR;
          }
        } while(error != CUDA_SUCCESS);
      }
    } 
  }

  struct ibv_send_wr *bad_wr;
  struct ibv_sge slistN;
  struct ibv_send_wr swrN;

  slistN.addr =
    (uintptr_t)(glb_gaspi_ctx_ib.nsrc.buf + notification_id * 4);

  *((unsigned int *) slistN.addr) = notification_value;

  slistN.length = 4;
  slistN.lkey = glb_gaspi_group_ib[0].mr->lkey;

  if((glb_gaspi_ctx_ib.rrmd[segment_id_remote][rank].cudaDevId >= 0))
  {
    swrN.wr.rdma.remote_addr = (glb_gaspi_ctx_ib.rrmd[segment_id_remote][rank].host_addr+notification_id*4);
    swrN.wr.rdma.rkey = glb_gaspi_ctx_ib.rrmd[segment_id_remote][rank].host_rkey;
  }
  else
  {
    swrN.wr.rdma.remote_addr =
      (glb_gaspi_ctx_ib.rrmd[segment_id_remote][rank].addr +
       notification_id * 4);
    swrN.wr.rdma.rkey = glb_gaspi_ctx_ib.rrmd[segment_id_remote][rank].rkey;
  }

  swrN.sg_list = &slistN;
  swrN.num_sge = 1;
  swrN.wr_id = rank;
  swrN.opcode = IBV_WR_RDMA_WRITE;
  swrN.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;;
  swrN.next = NULL;

  if (ibv_post_send (glb_gaspi_ctx_ib.qpC[queue][rank], &swrN, &bad_wr))
  {
    glb_gaspi_ctx.qp_state_vec[queue][rank] = 1;
    unlock_gaspi (&glb_gaspi_ctx.lockC[queue]);
    return GASPI_ERROR;
  }

  glb_gaspi_ctx_ib.ne_count_c[queue]++;        

  unlock_gaspi (&glb_gaspi_ctx.lockC[queue]);

  return GASPI_SUCCESS; 
}
