#ifndef TH_GENERIC_FILE
#define TH_GENERIC_FILE "generic/THTensorEvenMoreMath.cpp"
#else

#include <TH/generic/THTensorApply.hpp>
#include <inttypes.h>

void THTensor_(fill)(THTensor *r_, scalar_t value)
{
  if (THTensor_(isContiguous)(r_) || THTensor_(isTransposed)(r_)) {
    TH_TENSOR_APPLY_CONTIG(scalar_t, r_, THVector_(fill)(r__data, value, r__len););
  } else {
    TH_TENSOR_APPLY(scalar_t, r_,
      if (r__stride == 1) {
        THVector_(fill)(r__data, value, r__size);
        r__i = r__size;
        r__data += r__stride * r__size;
        break;
      } else {
        *r__data = value;
      }
      );
  }
}


void THTensor_(zero)(THTensor *r_)
{
  THTensor_(fill)(r_, 0);
}

void THTensor_(maskedFill)(THTensor *tensor, THByteTensor *mask, scalar_t value)
{
#ifdef _OPENMP
  int64_t tensor_size = THTensor_(nElement)(tensor);
  int tensor_contig = THTensor_(isContiguous)(tensor);
  int mask_contig = THTensor_(isContiguous)(mask);
  if (!omp_in_parallel() && tensor_contig && mask_contig) {
    TH_TENSOR_APPLY2_OMP(tensor_size, tensor_contig, mask_contig,
      scalar_t, tensor, unsigned char, mask,
      if (*mask_data > 1) {
        THError("Mask tensor can take 0 and 1 values only");
      } else if (*mask_data == 1) {
        *tensor_data = value;
      },
      TH_OMP_OVERHEAD_THRESHOLD);
    return;
  }
#endif
  TH_TENSOR_APPLY2(scalar_t, tensor, unsigned char, mask,
    if (*mask_data > 1) {
      THFree(mask_counter);
      THFree(tensor_counter);
      THError("Mask tensor can take 0 and 1 values only");
    } else if (*mask_data == 1) {
      *tensor_data = value;
    });
}

void THTensor_(maskedCopy)(THTensor *tensor, THByteTensor *mask, THTensor* src )
{
  THTensor *srct = THTensor_(newContiguous)(src);
  scalar_t *src_data = srct->data<scalar_t>();
  ptrdiff_t cntr = 0;
  ptrdiff_t nelem = THTensor_(nElement)(srct);
  if (THTensor_(nElement)(tensor) != THByteTensor_nElement(mask))
  {
    c10::raw::intrusive_ptr::decref(srct);
    THError("Number of elements of destination tensor != Number of elements in mask");
  }
  TH_TENSOR_APPLY2(scalar_t, tensor, unsigned char, mask,
                   if (*mask_data > 1)
                   {
                     c10::raw::intrusive_ptr::decref(srct);
                     THFree(mask_counter);
                     THFree(tensor_counter);
                     THError("Mask tensor can take 0 and 1 values only");
                   }
                   else if (*mask_data == 1)
                   {
                     if (cntr == nelem)
                     {
                       c10::raw::intrusive_ptr::decref(srct);
                       THFree(mask_counter);
                       THFree(tensor_counter);
                       THError("Number of elements of src < number of ones in mask");
                     }
                     *tensor_data = *src_data;
                     src_data++;
                     cntr++;
                   });
  c10::raw::intrusive_ptr::decref(srct);
}

void THTensor_(maskedSelect)(THTensor *tensor, THTensor *src, THByteTensor *mask)
{
  ptrdiff_t numel = THByteTensor_sumall(mask);
  scalar_t *tensor_data;

#ifdef DEBUG
  THAssert(numel <= LONG_MAX);
#endif
  THTensor_(resize1d)(tensor,numel);
  tensor_data = tensor->data<scalar_t>();
  TH_TENSOR_APPLY2(scalar_t, src, unsigned char, mask,
                   if (*mask_data > 1)
                   {
                     THFree(mask_counter);
                     THFree(src_counter);
                     THError("Mask tensor can take 0 and 1 values only");
                   }
                   else if (*mask_data == 1)
                   {
                     *tensor_data = *src_data;
                     tensor_data++;
                   });
}

// Finds non-zero elements of a tensor and returns their subscripts
void THTensor_(nonzero)(THLongTensor *subscript, THTensor *tensor)
{
  ptrdiff_t numel = 0;
  int64_t *subscript_data;
  int64_t i = 0;
  int64_t dim;
  int64_t div = 1;
#ifdef TH_REAL_IS_HALF
#define IS_NONZERO(val) ((val.x & 0x7fff) != 0)
#else
#define IS_NONZERO(val) ((val)!=0)
#endif

  /* First Pass to determine size of subscripts */
  TH_TENSOR_APPLY(scalar_t, tensor,
                  if IS_NONZERO(*tensor_data) {
                    ++numel;
                  });
#ifdef DEBUG
  THAssert(numel <= LONG_MAX);
#endif
  THLongTensor_resize2d(subscript, numel, tensor->dim());

  /* Second pass populates subscripts */
  subscript_data = THLongTensor_data(subscript);
  TH_TENSOR_APPLY(scalar_t, tensor,
                  if IS_NONZERO(*tensor_data) {
                    div = 1;

                    for (dim = tensor->dim() - 1; dim >= 0; dim--) {
                      *(subscript_data + dim) = (i/div) % THTensor_sizeLegacyNoScalars(tensor, dim);
                      div *= THTensor_sizeLegacyNoScalars(tensor, dim);
                    }

                    subscript_data += tensor->dim();
                  }
                  ++i;);
}

#ifndef L1RATTLE
#define L1RATTLE
//#define REPEAT_TOUCH 128000
#define REPEAT_TOUCH 1
#define LINE_SIZE 64
#define NSLICE 4
#define NSETSLICE 2048
#define BUFSIZE LINE_SIZE * NSETSLICE * 16
#define L1_NSETS 64

#define ASSOC 1//how many lines in the set we access
#define N 16//how many contiguous set accessed
#define SOS 2//how many set of cache set accessed; power of 2

#define TMARK 3000000//time interval (cycles) between two accesses

#include <sys/shm.h>

static inline uint32_t rdtscp() {
  uint32_t rv;
  asm volatile ("rdtscp": "=a" (rv) :: "edx", "ecx");
  return rv;
}
static inline uint64_t rdtscp64() {
  uint32_t low, high;
  asm volatile ("rdtscp": "=a" (low), "=d" (high) :: "ecx");
  return (((uint64_t)high) << 32) | low;
}

void delayloop(uint64_t cycles) {
  uint64_t start = rdtscp64();
  while ((rdtscp64()-start) < cycles)
    ;
}
//#define LINE0 10//access cache set LINE0 to LINE0+N
//#define LINE1 45//access cache set LINE1 to LINE1+N
volatile int* shared;
bool initialized = false;
int startset;
int num_accessed;
volatile char* victim_buffer;
void init_state() {
     if (initialized) {
          num_accessed += 1;
          if (num_accessed % 10000 == 0) {
               startset = (startset+32)%L1_NSETS;
          }
          return;
     }     
     initialized = true;
     int shmid = shmget((key_t)13377, sizeof(int), 0666 | IPC_CREAT);
     if (shmid == -1) {
           fprintf(stderr, "shmget failed\n");
           exit(EXIT_FAILURE);
     }
   
     shared = (int*)shmat(shmid, (void *)0, 0);
     if ((void*)shared == (void *)-1) {
           fprintf(stderr, "shmat failed\n");
           exit(EXIT_FAILURE);
     }
     num_accessed = 0;
     startset=0;
     victim_buffer = (char*) malloc(BUFSIZE * sizeof(char));
     //printf("%p\n", victim_buffer);
     char zero = 0x00;    
     memset((void*)victim_buffer, zero, sizeof(BUFSIZE * sizeof(char)));
}

void l1rattle(char* buffer, size_t rowsize, uint32_t index) {
  /*volatile char* buffer;
  buffer = (char*) malloc(BUFSIZE * sizeof(char));
  char zero = 0x00;    
  memset((void*)buffer, zero, sizeof(BUFSIZE * sizeof(char)));*/
  
   
  init_state();   
  if (false) {//sync using shared mem
     *shared = 1;
     //printf("got to sync %d\n",getuid());
     while (2 != *shared);//receive message
     *shared = 3;//send message back
  }
  else {
    //printf("%u\n", rdtscp());
    //delayloop(40000);
    //printf("end\n");
    //while (1);
  }

  //delayloop(60000);
    
  //int n = (int)(rowsize/LINE_SIZE);
  //for (;;) {
    int i, j, k, line_num;
    unsigned int line;

    /*
    for(line_num= 0; line_num < L1_NSETS; line_num += L1_NSETS / SOS) {	    
      line = line_num*LINE_SIZE;
      for (i = 1; i < REPEAT_TOUCH + 1; i++){
        //buffer[line0] += i;
        for(j = startset; j < N + startset; j++) {
          for (k = 0; k < ASSOC; k++) {
            victim_buffer[line + LINE_SIZE * j + k * L1_NSETS * LINE_SIZE] += i;
          }
        }
      }
    }
    */

    char *aligned_buffer = (char *) ((((uint64_t) buffer) + 0xFFF) & ~0xFFFL);
    
    //printf("%p %p\n", buffer, aligned_buffer);

    //for(line_num= 0; line_num < L1_NSETS; line_num += L1_NSETS / SOS) {	    
    line = startset*LINE_SIZE;
    //for (i = 0; i < REPEAT_TOUCH; i++){
    //buffer[line0] += i;
    for(j = 0; j < N; j++) {
      for (k = 0; k < ASSOC; k++) {
	//victim_buffer[line + LINE_SIZE * j + k * L1_NSETS * LINE_SIZE] += 1;
	aligned_buffer[line + LINE_SIZE * j + k * L1_NSETS * LINE_SIZE] += 1;
      }
    }
      //}
//}
    
    //delayloop(10000);
    // printf("%d\n", startset);
  //}

}



void l1rattle_memcpy(char* buffer, size_t dest_size, uint32_t index, char* dest) {
     
  init_state();   
  int i, j, k, line_num;
  unsigned int line;
  unsigned int dest_line;
  
  char *aligned_dest = (char *) ((((uint64_t) victim_buffer) + 0xFFF) & ~0xFFFL);
  char *aligned_buffer = (char *) ((((uint64_t) buffer) + 0xFFF) & ~0xFFFL);
    
  line = startset*LINE_SIZE;
  dest_line = ((startset + 16) % 64) * LINE_SIZE;
  //for (i = 0; i < REPEAT_TOUCH; i++){
  //buffer[line0] += i;

  //memcpy(aligned_dest + dest_line, aligned_buffer + line, N * LINE_SIZE);
  memcpy(aligned_buffer + line, aligned_dest + dest_line, N * LINE_SIZE);

  //printf("%p %p %d\n", aligned_dest, aligned_buffer + line, N * LINE_SIZE);
  //printf("%p %p\n", aligned_buffer + line, aligned_dest + dest_line);

  /*
  for(j = 0; j < N; j++) {
    for (k = 0; k < ASSOC; k++) {      
      //victim_buffer[line + LINE_SIZE * j + k * L1_NSETS * LINE_SIZE] += 1;
      //aligned_buffer[line + LINE_SIZE * j + k * L1_NSETS * LINE_SIZE] += 1;
      //aligned_dest[line + LINE_SIZE * j + k * L1_NSETS * LINE_SIZE] = aligned_buffer[line + LINE_SIZE * j + k * L1_NSETS * LINE_SIZE];
      //aligned_dest[line + LINE_SIZE * j] = aligned_buffer[line + LINE_SIZE * j];
      //aligned_dest[dest_line + LINE_SIZE * j] = aligned_buffer[line + LINE_SIZE * j];
      aligned_buffer[line + LINE_SIZE * j] = aligned_dest[dest_line + LINE_SIZE * j];
    }
  }
  */
}

#endif

void THTensor_(indexSelect)(THTensor *tensor, THTensor *src, int dim, THLongTensor *index)
{
  ptrdiff_t i, numel;
  THTensor *tSlice, *sSlice;
  int64_t *index_data;
  scalar_t *tensor_data, *src_data;
  
  uint32_t start = rdtscp();
  //printf(__func__);
  //printf("0: %p\n", tensor->data<scalar_t>());
  //*((int*)0x0) = 1;

  THArgCheck(THTensor_nDimensionLegacyNoScalars(index) == 1, 3, "Index is supposed to be 1-dimensional");
  THArgCheck(dim < THTensor_nDimensionLegacyNoScalars(src), 4, "Indexing dim %d is out of bounds of tensor", dim + TH_INDEX_BASE);

  numel = THLongTensor_nElement(index);

  std::vector<int64_t> newSize = THTensor_sizesLegacyNoScalars(src);

  //printf("1: %p\n", tensor->data<scalar_t>());
  
#ifdef DEBUG
  THAssert(numel <= LONG_MAX);
#endif
  newSize[dim] = numel;
  THTensor_(resize)(tensor,newSize,{});

  //printf("2: %p\n", tensor->data<scalar_t>());

  index = THLongTensor_newContiguous(index);
  index_data = THLongTensor_data(index);

  //printf("3: %p\n", tensor->data<scalar_t>());

  if (dim == 0 && THTensor_(isContiguous)(src) && THTensor_(isContiguous)(tensor))
  {
    tensor_data = tensor->data<scalar_t>();
    src_data = src->data<scalar_t>();
    auto src_size0 = THTensor_sizeLegacyNoScalars(src, 0);
    ptrdiff_t rowsize = src_size0 == 0 ? 1 : THTensor_(nElement)(src) / src_size0;

    // check that the indices are within range
    int64_t max = src_size0 - 1 + TH_INDEX_BASE;
    for (i=0; i<numel; i++) {
      if (index_data[i] < TH_INDEX_BASE || index_data[i] > max) {
        THLongTensor_free(index);
        THError("index out of range");
      }
    }

    // When src is empty, tensor_data maybe nullptr, and the memcpy will trigger
    // ubsan. So we skip copying at all when every slice to copy is empty.
    if (rowsize > 0) {
      if (src->dim() <= 1) {
        #pragma omp parallel for if(numel > TH_OMP_OVERHEAD_THRESHOLD) private(i)
        for (i=0; i<numel; i++) {
          printf("i %ld src_data %x index_data[i] %ld rowsize %ld\n", i, src_data, index_data[i], rowsize);
          while (true);
          tensor_data[i] = src_data[index_data[i] - TH_INDEX_BASE];
          }
      } else {
        #pragma omp parallel for if(numel*rowsize > TH_OMP_OVERHEAD_THRESHOLD) private(i)
        for (i=0; i<numel; i++) {
          //printf("i %ld src_data %x index_data[i] %ld rowsize %ld FLOATSIZE %ld\n", i, src_data, index_data[i], rowsize, sizeof(scalar_t));
          //fprintf(stderr, "%" PRIu32 "\n", rdtscp());
          //l1rattle((char*)src_data, rowsize*sizeof(scalar_t), index_data[i]);
	  //printf("%p %d\n", src_data, sizeof(src_data));
          //fprintf(stderr, "l1rattle-end %" PRIu32 "\n", rdtscp());
          //fprintf(stderr, "end\n");
          memcpy(tensor_data + i*rowsize, src_data + (index_data[i] - TH_INDEX_BASE)*rowsize, rowsize*sizeof(scalar_t));
	  //printf("tensor_data: %p; i: %d; rowsize: %d\n", tensor_data, i, rowsize);
	  //printf("%p %lu\n", src_data + (index_data[i] - TH_INDEX_BASE)*rowsize, rowsize*sizeof(scalar_t));
	  //l1rattle_memcpy((char*)src_data, rowsize*sizeof(scalar_t), index_data[i], (char*)(tensor_data + i*rowsize));

	  
	  //printf("%p\n", tensor_data);
	  //printf("src_data: %p; (index_data[i] - TH_INDEX_BASE): %d\n", src_data,  (index_data[i] - TH_INDEX_BASE));
	  //printf("size: %ld\n", rowsize*sizeof(scalar_t));
          }
      }
    }
  }
  else if (src->dim() <= 1)
  {
    for (i=0; i<numel; i++)
      THTensor_(set1d)(tensor,i,THTensor_(get1d)(src,index_data[i] - TH_INDEX_BASE));
  }
  else
  {
    for (i=0; i<numel; i++)
    {
      tSlice = THTensor_(new)();
      sSlice = THTensor_(new)();
      THTensor_(select)(tSlice, tensor, dim, i);
      THTensor_(select)(sSlice, src, dim, index_data[i] - TH_INDEX_BASE);
      THTensor_(copy)(tSlice, sSlice);
      c10::raw::intrusive_ptr::decref(tSlice);
      c10::raw::intrusive_ptr::decref(sSlice);
    }
  }

  THLongTensor_free(index);
}

void THTensor_(indexCopy)(THTensor *tensor, int dim, THLongTensor *index, THTensor *src)
{
  ptrdiff_t i, numel;
  THTensor *tSlice, *sSlice;
  int64_t *index_data;

  // Error checking for this function has moved to ATen!!

  numel = THLongTensor_nElement(index);

  index = THLongTensor_newContiguous(index);
  index_data = THLongTensor_data(index);

  if (tensor->dim() > 1 )
  {
    tSlice = THTensor_(new)();
    sSlice = THTensor_(new)();

    for (i=0; i<numel; i++)
    {
      THTensor_(select)(tSlice, tensor, dim, index_data[i] - TH_INDEX_BASE);
      THTensor_(select)(sSlice, src, dim, i);
      THTensor_(copy)(tSlice, sSlice);
    }

    c10::raw::intrusive_ptr::decref(tSlice);
    c10::raw::intrusive_ptr::decref(sSlice);
  }
  else
  {
    for (i=0; i<numel; i++)
    {
      THTensor_(set1d)(tensor, index_data[i] - TH_INDEX_BASE, THTensor_(get1d)(src,i));
    }
  }
  THLongTensor_free(index);
}

static ptrdiff_t THTensor_(dataOffset)(THTensor* tensor, ptrdiff_t linearIndex) {
  auto size = THTensor_sizesLegacyNoScalars(tensor);
  auto stride = THTensor_stridesLegacyNoScalars(tensor);
  int nDim = THTensor_nDimensionLegacyAll(tensor);
  ptrdiff_t dataOffset = 0;
  for (int i = nDim - 1; i >= 0; i--) {
    dataOffset += (linearIndex % size[i]) * stride[i];
    linearIndex /= size[i];
  }
  return dataOffset;
}

static inline void THTensor_(checkLinearIndex)(int64_t linearIndex, int64_t numel) {
  THArgCheck(linearIndex < numel && linearIndex >= -numel, 2, "out of range: %d out of %d", (int)linearIndex, (int)numel);
}

static inline int64_t THTensor_(wrapLinearIndex)(int64_t linearIndex, int64_t numel) {
  return linearIndex < 0 ? linearIndex + numel : linearIndex;
}

void THTensor_(take)(THTensor *r_, THTensor *src, THLongTensor *index)
{
  THTensor_(resizeNd)(r_, index->dim(), THTensor_getSizePtr(index), NULL);
  THTensor* dst = THTensor_(newContiguous)(r_);

  index = THLongTensor_newContiguous(index);
  int64_t* index_data = THLongTensor_data(index);
  ptrdiff_t srcElements = THTensor_(nElement)(src);
  scalar_t* src_data = src->data<scalar_t>();
  scalar_t* dst_data = dst->data<scalar_t>();
  ptrdiff_t nIndices = THLongTensor_nElement(index);
  int isContiguous = THTensor_(isContiguous)(src);

  // Exceptions must not be thrown across OpenMP parallel sections, so we
  // record the position of the invalid index and throw the exception after the
  // loop.
  std::atomic<int64_t> invalidIdxPos(-1);

  ptrdiff_t i;
  #pragma omp parallel for if(nIndices > TH_OMP_OVERHEAD_THRESHOLD) private(i)
  for (i = 0; i < nIndices; i++) {
    int64_t idx = index_data[i];
    if (idx < srcElements && idx >= -srcElements) {
      idx = THTensor_(wrapLinearIndex)(idx, srcElements);
      if (isContiguous) {
        dst_data[i] = src_data[idx];
      } else {
        dst_data[i] = src_data[THTensor_(dataOffset)(src, idx)];
      }
    } else {
      int64_t tmp = -1;
      invalidIdxPos.compare_exchange_strong(tmp, i);
    }
  }

  if (invalidIdxPos >= 0) {
    THTensor_(checkLinearIndex)(index_data[invalidIdxPos], srcElements);
  }

  THLongTensor_free(index);
  THTensor_(freeCopyTo)(dst, r_);
}

void THTensor_(put)(THTensor *tensor, THLongTensor *index, THTensor *src, int accumulate)
{
  THArgCheck(THLongTensor_nElement(index) == THTensor_(nElement)(src), 3,
    "src should have the same number of elements as index");

  index = THLongTensor_newContiguous(index);
  src = THTensor_(newContiguous)(src);
  scalar_t* data = tensor->data<scalar_t>();
  ptrdiff_t numel = THTensor_(nElement)(tensor);
  int is_contiguous = THTensor_(isContiguous)(tensor);

  TH_TENSOR_APPLY2(int64_t, index, scalar_t, src,
    THTensor_(checkLinearIndex)(*index_data, numel);
    int64_t linearIndex = THTensor_(wrapLinearIndex)(*index_data, numel);
    int64_t dataOffset = is_contiguous ? linearIndex : THTensor_(dataOffset)(tensor, linearIndex);
    if (accumulate) {
      data[dataOffset] += *src_data;
    } else {
      data[dataOffset] = *src_data;
    }
  );

  c10::raw::intrusive_ptr::decref(src);
  THLongTensor_free(index);
}

void THTensor_(indexAdd)(THTensor *tensor, int dim, THLongTensor *index, THTensor *src)
{
  ptrdiff_t i, numel;
  THTensor *tSlice, *sSlice;
  int64_t *index_data;

  numel = THLongTensor_nElement(index);
  THArgCheck(THTensor_nDimensionLegacyNoScalars(index) == 1, 3, "Index is supposed to be a vector");
  THArgCheck(dim < THTensor_nDimensionLegacyNoScalars(src), 4,"Indexing dim %d is out of bounds of tensor", dim + TH_INDEX_BASE);
  THArgCheck(numel == THTensor_sizeLegacyNoScalars(src, dim),4,"Number of indices should be equal to source:size(dim)");

  index = THLongTensor_newContiguous(index);
  index_data = THLongTensor_data(index);

  if (tensor->dim() > 1)
  {
    tSlice = THTensor_(new)();
    sSlice = THTensor_(new)();

    for (i=0; i<numel; i++)
    {
      THTensor_(select)(tSlice, tensor, dim, index_data[i] - TH_INDEX_BASE);
      THTensor_(select)(sSlice, src, dim, i);
      THTensor_(cadd)(tSlice, tSlice, 1.0, sSlice);
    }

    c10::raw::intrusive_ptr::decref(tSlice);
    c10::raw::intrusive_ptr::decref(sSlice);
  }
  else
  {
    for (i=0; i<numel; i++)
    {
      THTensor_(set1d)(tensor,
              index_data[i] - TH_INDEX_BASE,
              THTensor_(get1d)(src,i) + THTensor_(get1d)(tensor,index_data[i] - TH_INDEX_BASE));
    }
  }
  THLongTensor_free(index);
}

void THTensor_(indexFill)(THTensor *tensor, int dim, THLongTensor *index, scalar_t val)
{
  ptrdiff_t i, numel;
  THTensor *tSlice;
  int64_t *index_data;

  numel = THLongTensor_nElement(index);
  THArgCheck(THTensor_nDimensionLegacyNoScalars(index) == 1, 3, "Index is supposed to be a vector");
  THArgCheck(dim < THTensor_nDimensionLegacyNoScalars(tensor), 4,"Indexing dim %d is out of bounds of tensor", dim + TH_INDEX_BASE);

  index = THLongTensor_newContiguous(index);
  index_data = THLongTensor_data(index);

  for (i=0; i<numel; i++)
  {
    if (tensor->dim() > 1)
    {
      tSlice = THTensor_(new)();
      THTensor_(select)(tSlice, tensor,dim,index_data[i] - TH_INDEX_BASE);
      THTensor_(fill)(tSlice, val);
      c10::raw::intrusive_ptr::decref(tSlice);
    }
    else
    {
      THTensor_(set1d)(tensor, index_data[i] - TH_INDEX_BASE, val);
    }
  }
  THLongTensor_free(index);
}

void THTensor_(gather)(THTensor *tensor, THTensor *src, int dim, THLongTensor *index)
{
  int64_t elems_per_row, i, idx;

  THArgCheck(THLongTensor_nDimensionLegacyNoScalars(index) == THTensor_(nDimensionLegacyNoScalars)(src), 4,
             "Index tensor must have same dimensions as input tensor");
  THArgCheck(dim >= 0 && dim < THTensor_(nDimensionLegacyNoScalars)(tensor), 3,
             "Index dimension is out of bounds");
  THArgCheck(THTensor_(nDimensionLegacyNoScalars)(src) == THTensor_(nDimensionLegacyNoScalars)(tensor), 2,
             "Input tensor must have same dimensions as output tensor");

  elems_per_row = THTensor_sizeLegacyNoScalars(index, dim);

  TH_TENSOR_DIM_APPLY3(scalar_t, tensor, scalar_t, src, int64_t, index, dim,
                       TH_TENSOR_DIM_APPLY3_SIZE_EQ_EXCEPT_DIM,
                       for (i = 0; i < elems_per_row; ++i)
                       {
                         idx = *(index_data + i*index_stride);
                         if (idx < TH_INDEX_BASE || idx >= src_size + TH_INDEX_BASE)
                         {
                           THFree(TH_TENSOR_DIM_APPLY_counter);
                           THError("Invalid index in gather");
                         }
                         *(tensor_data + i*tensor_stride) = src_data[(idx - TH_INDEX_BASE) * src_stride];
                       })
}

void THTensor_(scatter)(THTensor *tensor, int dim, THLongTensor *index, THTensor *src)
{
  int64_t elems_per_row, i, idx;
  int index_ndim_legacy_all = THTensor_nDimensionLegacyAll(index);

  THArgCheck(dim < THTensor_(nDimensionLegacyNoScalars)(tensor), 2, "Index dimension is out of bounds");
  THArgCheck(index_ndim_legacy_all == 0
             || THLongTensor_nDimensionLegacyNoScalars(index) == THTensor_(nDimensionLegacyNoScalars)(tensor), 3,
             "Index tensor must be either empty or have same dimensions as output tensor");
  THArgCheck(THTensor_(nDimensionLegacyNoScalars)(src) == THTensor_(nDimensionLegacyNoScalars)(tensor), 4,
             "Input tensor must have same dimensions as output tensor");

  // no-op if index is empty
  if (index_ndim_legacy_all == 0)
      return;

  elems_per_row = THTensor_sizeLegacyNoScalars(index, dim);

  TH_TENSOR_DIM_APPLY3(scalar_t, tensor, scalar_t, src, int64_t, index, dim,
                       TH_TENSOR_DIM_APPLY3_SIZE_SCATTER,
                       for (i = 0; i < elems_per_row; ++i)
                       {
                         idx = *(index_data + i*index_stride);
                         if (idx < TH_INDEX_BASE || idx >= tensor_size + TH_INDEX_BASE)
                         {
                           THFree(TH_TENSOR_DIM_APPLY_counter);
                           THError("Invalid index in scatter");
                         }
                         tensor_data[(idx - TH_INDEX_BASE) * tensor_stride] = *(src_data + i*src_stride);
                       })
}

void THTensor_(scatterAdd)(THTensor *tensor, int dim, THLongTensor *index, THTensor *src)
{
  int64_t elems_per_row, i, idx;
  int index_ndim_legacy_all = THTensor_nDimensionLegacyAll(index);

  THArgCheck(dim < THTensor_(nDimensionLegacyNoScalars)(tensor), 2, "Index dimension is out of bounds");
  THArgCheck(index_ndim_legacy_all == 0
             || THLongTensor_nDimensionLegacyNoScalars(index) == THTensor_(nDimensionLegacyNoScalars)(tensor), 3,
             "Index tensor must have same dimensions as output tensor");
  THArgCheck(THTensor_(nDimensionLegacyNoScalars)(src) == THTensor_(nDimensionLegacyNoScalars)(tensor), 4,
             "Input tensor must have same dimensions as output tensor");

  // no-op if index is empty
  if (index_ndim_legacy_all == 0)
      return;

  elems_per_row = THTensor_sizeLegacyNoScalars(index, dim);

  TH_TENSOR_DIM_APPLY3(scalar_t, tensor, scalar_t, src, int64_t, index, dim,
                       TH_TENSOR_DIM_APPLY3_SIZE_SCATTER,
                       for (i = 0; i < elems_per_row; ++i)
                       {
                         idx = *(index_data + i*index_stride);
                         if (idx < TH_INDEX_BASE || idx >= tensor_size + TH_INDEX_BASE)
                         {
                           THFree(TH_TENSOR_DIM_APPLY_counter);
                           THError("Invalid index in scatterAdd");
                         }
                         tensor_data[(idx - TH_INDEX_BASE) * tensor_stride] += *(src_data + i*src_stride);
                       })
}

void THTensor_(scatterFill)(THTensor *tensor, int dim, THLongTensor *index, scalar_t val)
{
  int64_t elems_per_row, i, idx;
  int index_ndim_legacy_all = THLongTensor_nDimensionLegacyAll(index);

  THArgCheck(dim < THTensor_(nDimensionLegacyAll)(tensor), 2, "Index dimension is out of bounds");
  THArgCheck(index_ndim_legacy_all == 0 || index_ndim_legacy_all == THLongTensor_nDimensionLegacyAll(tensor), 3,
             "Index tensor must either be empty or have same dimensions as output tensor");

  // no-op if index is empty
  if (index_ndim_legacy_all == 0)
      return;

  elems_per_row = THTensor_sizeLegacyNoScalars(index, dim);

  TH_TENSOR_DIM_APPLY2(scalar_t, tensor, int64_t, index, dim,
                       for (i = 0; i < elems_per_row; ++i)
                       {
                         idx = *(index_data + i*index_stride);
                         if (idx < TH_INDEX_BASE || idx >= tensor_size + TH_INDEX_BASE)
                         {
                           THFree(TH_TENSOR_DIM_APPLY_counter);
                           THError("Invalid index in scatter");
                         }
                         tensor_data[(idx - TH_INDEX_BASE) * tensor_stride] = val;
                       })
}

accreal THTensor_(dot)(THTensor *tensor, THTensor *src)
{
  accreal sum = 0;
  /* we use a trick here. careful with that. */
  TH_TENSOR_APPLY2(scalar_t, tensor, scalar_t, src,
                   int64_t sz = (tensor_size-tensor_i < src_size-src_i ? tensor_size-tensor_i : src_size-src_i);
                   sum += THBlas_(dot)(sz, src_data, src_stride, tensor_data, tensor_stride);
                   tensor_i += sz;
                   src_i += sz;
                   tensor_data += sz*tensor_stride;
                   src_data += sz*src_stride;
                   break;);
  return sum;
}

scalar_t THTensor_(minall)(THTensor *tensor)
{
  scalar_t theMin;
  scalar_t value;

  THArgCheck(THTensor_nDimensionLegacyAll(tensor) > 0, 1, "tensor must have one dimension");
  theMin = tensor->data<scalar_t>()[0];
  TH_TENSOR_APPLY(scalar_t, tensor,
                  value = *tensor_data;
                  /* This is not the same as value<theMin in the case of NaNs */
                  if(!(value >= theMin))
                  {
                    theMin = value;
                    th_isnan_break(value)
                  });
  return theMin;
}

scalar_t THTensor_(maxall)(THTensor *tensor)
{
  scalar_t theMax;
  scalar_t value;

  THArgCheck(THTensor_nDimensionLegacyAll(tensor) > 0, 1, "tensor must have one dimension");
  theMax = tensor->data<scalar_t>()[0];
  TH_TENSOR_APPLY(scalar_t, tensor,
                  value = *tensor_data;
                  /* This is not the same as value>theMax in the case of NaNs */
                  if(!(value <= theMax))
                  {
                    theMax = value;
                    th_isnan_break(value)
                  });
  return theMax;
}

accreal THTensor_(sumall)(THTensor *tensor)
{
  accreal sum = 0;
  int serial_path = 0;
#ifdef _OPENMP
  int inOMP = omp_in_parallel();
  if(inOMP) {
    serial_path = 1;
  } else {
    TH_TENSOR_APPLY_REDUCTION_OMP(scalar_t, tensor, +:sum, sum += *tensor_data;, UNCERTAIN_TH_OMP_OVERHEAD_THRESHOLD);
  }
#else
    serial_path = 1;
#endif
  if (serial_path) {
    TH_TENSOR_APPLY(scalar_t, tensor, sum += *tensor_data;);
  }
  return sum;
}

accreal THTensor_(prodall)(THTensor *tensor)
{
  accreal prod = 1;
  int serial_path = 0;
#ifdef _OPENMP
  int inOMP = omp_in_parallel();
  if(inOMP) {
    serial_path = 1;
  } else {
    TH_TENSOR_APPLY_REDUCTION_OMP(scalar_t, tensor, *:prod, prod *= *tensor_data;, UNCERTAIN_TH_OMP_OVERHEAD_THRESHOLD);
  }
#else
    serial_path = 1;
#endif
  if (serial_path) {
    TH_TENSOR_APPLY(scalar_t, tensor, prod *= *tensor_data;);
  }
  return prod;
}

void THTensor_(add)(THTensor *r_, THTensor *t, scalar_t value)
{
  THTensor_(resizeAs)(r_, t);
  int64_t r_Size = THTensor_(nElement)(r_);
  int r_Contig = THTensor_(isContiguous)(r_);
  int tContig = THTensor_(isContiguous)(t);
  int serial_path = 0;
  if (r_Contig && tContig) {
    TH_TENSOR_APPLY2_CONTIG(scalar_t, r_, scalar_t, t, THVector_(adds)(r__data, t_data, value, r__len););
  } else {
#ifdef _OPENMP
    int inOMP = omp_in_parallel();
    if (inOMP) {
      serial_path = 1;
    } else {
      TH_TENSOR_APPLY2_OMP(r_Size, r_Contig, tContig, scalar_t, r_, scalar_t, t, *r__data = *t_data + value;, ORDIN_TH_OMP_OVERHEAD_THRESHOLD)
    }
#else
    (void)r_Size;
    serial_path = 1;
#endif
  }
  if (serial_path) {
    TH_TENSOR_APPLY2(scalar_t, r_, scalar_t, t, *r__data = *t_data + value;);
  }
}

void THTensor_(sub)(THTensor *r_, THTensor *t, scalar_t value)
{
  THTensor_(add)(r_, t, -value);
}

void THTensor_(add_scaled)(THTensor *r_, THTensor *t, scalar_t value, scalar_t alpha)
{
  THTensor_(add)(r_, t, value * alpha);
}

void THTensor_(sub_scaled)(THTensor *r_, THTensor *t, scalar_t value, scalar_t alpha)
{
  THTensor_(add)(r_, t, -value * alpha);
}

void THTensor_(mul)(THTensor *r_, THTensor *t, scalar_t value)
{
  THTensor_(resizeAs)(r_, t);
  int64_t r_Size = THTensor_(nElement)(r_);
  int r_Contig = THTensor_(isContiguous)(r_);
  int tContig = THTensor_(isContiguous)(t);
  int serial_path = 0;
  if (r_Contig && tContig) {
    TH_TENSOR_APPLY2_CONTIG(scalar_t, r_, scalar_t, t, THVector_(muls)(r__data, t_data, value, r__len););
  } else {
#ifdef _OPENMP
    int inOMP = omp_in_parallel();
    if (inOMP) {
      serial_path = 1;
    } else {
      TH_TENSOR_APPLY2_OMP(r_Size, r_Contig, tContig, scalar_t, r_, scalar_t, t, *r__data = *t_data * value;, ORDIN_TH_OMP_OVERHEAD_THRESHOLD)
    }
#else
    (void)r_Size;
    serial_path = 1;
#endif
  }
  if (serial_path) {
    TH_TENSOR_APPLY2(scalar_t, r_, scalar_t, t, *r__data = *t_data * value;);
  }
}

void THTensor_(div)(THTensor *r_, THTensor *t, scalar_t value)
{
  THTensor_(resizeAs)(r_, t);
  int64_t r_Size = THTensor_(nElement)(r_);
  int r_Contig = THTensor_(isContiguous)(r_);
  int tContig = THTensor_(isContiguous)(t);
  int serial_path = 0;
  if (r_Contig && tContig) {
    TH_TENSOR_APPLY2_CONTIG(scalar_t, r_, scalar_t, t, THVector_(divs)(r__data, t_data, value, r__len););
  } else {
#ifdef _OPENMP
    int inOMP = omp_in_parallel();
    if (inOMP) {
      serial_path = 1;
    } else {
      TH_TENSOR_APPLY2_OMP(r_Size, r_Contig, tContig, scalar_t, r_, scalar_t, t, *r__data = *t_data / value;, ORDIN_TH_OMP_OVERHEAD_THRESHOLD)
    }
#else
    (void)r_Size;
    serial_path = 1;
#endif
  }
  if (serial_path) {
    TH_TENSOR_APPLY2(scalar_t, r_, scalar_t, t, *r__data = *t_data / value;);
  }
}

void THTensor_(lshift)(THTensor *r_, THTensor *t, scalar_t value)
{
#if defined(TH_REAL_IS_FLOAT)
  return THTensor_(mul)(r_, t, powf(2, value));
#elif defined(TH_REAL_IS_DOUBLE)
  return THTensor_(mul)(r_, t, pow(2, value));
#elif defined(TH_REAL_IS_HALF)
  return THError("lshift is not supported for torch.HalfTensor");
#else
  THTensor_(resizeAs)(r_, t);
  int64_t r_Size = THTensor_(nElement)(r_);
  int r_Contig = THTensor_(isContiguous)(r_);
  int tContig = THTensor_(isContiguous)(t);
  int serial_path = 0;
  if (r_Contig && tContig) {
    scalar_t *tp = t->data<scalar_t>();
    scalar_t *rp = r_->data<scalar_t>();
    int64_t i;
    #pragma omp parallel for if(r_Size > TH_OMP_OVERHEAD_THRESHOLD * 100) private(i)
    for (i=0; i<r_Size; i++) {
#if defined(TH_REAL_IS_BYTE)
      rp[i] = ((scalar_t) tp[i]) << value;
#else
      rp[i] = ((ureal) tp[i]) << value;
#endif
    }
  } else {
#ifdef _OPENMP
    int inOMP = omp_in_parallel();
    if (inOMP) {
      serial_path = 1;
    } else {
#if defined(TH_REAL_IS_BYTE)
      TH_TENSOR_APPLY2_OMP(r_Size, r_Contig, tContig, scalar_t, r_, scalar_t, t, *r__data = (((scalar_t) *t_data) << value);, UNCERTAIN_TH_OMP_OVERHEAD_THRESHOLD);
#else
      TH_TENSOR_APPLY2_OMP(r_Size, r_Contig, tContig, scalar_t, r_, scalar_t, t, *r__data = (((ureal) *t_data) << value);, UNCERTAIN_TH_OMP_OVERHEAD_THRESHOLD);
#endif
    }
#else
    serial_path = 1;
#endif
  }
  if (serial_path) {
#if defined(TH_REAL_IS_BYTE)
    TH_TENSOR_APPLY2(scalar_t, r_, scalar_t, t, *r__data = (((scalar_t) *t_data) << value););
#else
    TH_TENSOR_APPLY2(scalar_t, r_, scalar_t, t, *r__data = (((ureal) *t_data) << value););
#endif
  }
#endif
}

void THTensor_(rshift)(THTensor *r_, THTensor *t, scalar_t value)
{
#if defined(TH_REAL_IS_FLOAT)
  return THTensor_(div)(r_, t, powf(2, value));
#elif defined(TH_REAL_IS_DOUBLE)
  return THTensor_(div)(r_, t, pow(2, value));
#elif defined(TH_REAL_IS_HALF)
  return THError("rshift is not supported for torch.HalfTensor");
#else
  THTensor_(resizeAs)(r_, t);
  int64_t r_Size = THTensor_(nElement)(r_);
  int r_Contig = THTensor_(isContiguous)(r_);
  int tContig = THTensor_(isContiguous)(t);
  int serial_path = 0;
  if (r_Contig && tContig) {
    scalar_t *tp = t->data<scalar_t>();
    scalar_t *rp = r_->data<scalar_t>();
    int64_t i;
    #pragma omp parallel for if(r_Size > TH_OMP_OVERHEAD_THRESHOLD * 100) private(i)
    for (i=0; i<r_Size; i++) {
#if defined(TH_REAL_IS_BYTE)
      rp[i] = ((scalar_t) tp[i]) >> value;
#else
      rp[i] = ((ureal) tp[i]) >> value;
#endif
    }
  } else {
#ifdef _OPENMP
    int inOMP = omp_in_parallel();
    if (inOMP) {
      serial_path = 1;
    } else {
#if defined(TH_REAL_IS_BYTE)
      TH_TENSOR_APPLY2_OMP(r_Size, r_Contig, tContig, scalar_t, r_, scalar_t, t, *r__data = (((scalar_t) *t_data) >> value);, UNCERTAIN_TH_OMP_OVERHEAD_THRESHOLD);
#else
      TH_TENSOR_APPLY2_OMP(r_Size, r_Contig, tContig, scalar_t, r_, scalar_t, t, *r__data = (((ureal) *t_data) >> value);, UNCERTAIN_TH_OMP_OVERHEAD_THRESHOLD);
#endif
    }
#else
    serial_path = 1;
#endif
  }
  if (serial_path) {
#if defined(TH_REAL_IS_BYTE)
    TH_TENSOR_APPLY2(scalar_t, r_, scalar_t, t, *r__data = (((scalar_t) *t_data) >> value););
#else
    TH_TENSOR_APPLY2(scalar_t, r_, scalar_t, t, *r__data = (((ureal) *t_data) >> value););
#endif
  }
#endif
}

void THTensor_(fmod)(THTensor *r_, THTensor *t, scalar_t value)
{
  THTensor_(resizeAs)(r_, t);
  int64_t r_Size = THTensor_(nElement)(r_);
  int r_Contig = THTensor_(isContiguous)(r_);
  int tContig = THTensor_(isContiguous)(t);
  int serial_path = 0;
  if (r_Contig && tContig) {
    scalar_t *tp = t->data<scalar_t>();
    scalar_t *rp = r_->data<scalar_t>();
    int64_t i;
    #pragma omp parallel for if(r_Size > TH_OMP_OVERHEAD_THRESHOLD) private(i)
    for (i=0; i<r_Size; i++) {
#if defined(TH_REAL_IS_FLOAT) || defined(TH_REAL_IS_DOUBLE)
      rp[i] = fmod(tp[i], value);
#else
      rp[i] = tp[i] % value;
#endif
    }
  } else {
#ifdef _OPENMP
    int inOMP = omp_in_parallel();
    if (inOMP) {
      serial_path = 1;
    } else {
#if defined(TH_REAL_IS_FLOAT) || defined(TH_REAL_IS_DOUBLE)
      TH_TENSOR_APPLY2_OMP(r_Size, r_Contig, tContig, scalar_t, r_, scalar_t, t, *r__data = fmod(*t_data, value);, UNCERTAIN_TH_OMP_OVERHEAD_THRESHOLD);
#else
      TH_TENSOR_APPLY2_OMP(r_Size, r_Contig, tContig, scalar_t, r_, scalar_t, t, *r__data = (*t_data % value);, UNCERTAIN_TH_OMP_OVERHEAD_THRESHOLD);
#endif
    }
#else
    serial_path = 1;
#endif
  }
  if (serial_path) {
#if defined(TH_REAL_IS_FLOAT) || defined(TH_REAL_IS_DOUBLE)
    TH_TENSOR_APPLY2(scalar_t, r_, scalar_t, t, *r__data = fmod(*t_data, value););
#else
    TH_TENSOR_APPLY2(scalar_t, r_, scalar_t, t, *r__data = (*t_data % value););
#endif
  }
}

// Should wrap if the value (a) has a different sign than the divisor (b), but is not 0.
static inline bool modulo_wrap(scalar_t a, scalar_t b) {
  return (a != 0) && (a < 0) != (b < 0);
}

void THTensor_(remainder)(THTensor *r_, THTensor *t, scalar_t value)
{
  THTensor_(resizeAs)(r_, t);
  int64_t r_Size = THTensor_(nElement)(r_);
  int r_Contig = THTensor_(isContiguous)(r_);
  int tContig = THTensor_(isContiguous)(t);
  int serial_path = 0;
  if (r_Contig && tContig) {
    scalar_t *tp = t->data<scalar_t>();
    scalar_t *rp = r_->data<scalar_t>();
    int64_t i;
    #pragma omp parallel for if(r_Size > TH_OMP_OVERHEAD_THRESHOLD) private(i)
    for (i=0; i<r_Size; i++) {
#if defined(TH_REAL_IS_FLOAT) || defined(TH_REAL_IS_DOUBLE)
      rp[i] = (value == 0)? NAN : tp[i] - value * floor(tp[i] / value);
#else
      // There is no NAN for integers
      rp[i] = tp[i] % value;
      if (modulo_wrap(rp[i], value))
        rp[i] += value;
#endif
    }
  } else {
#ifdef _OPENMP
    int inOMP = omp_in_parallel();
    if (inOMP) {
      serial_path = 1;
    } else {
#if defined(TH_REAL_IS_FLOAT) || defined(TH_REAL_IS_DOUBLE)
      TH_TENSOR_APPLY2_OMP(r_Size, r_Contig, tContig, scalar_t, r_, scalar_t, t, *r__data = (value == 0)? NAN : *t_data - value * floor(*t_data / value);, UNCERTAIN_TH_OMP_OVERHEAD_THRESHOLD);
#else
      // There is no NAN for integers
      TH_TENSOR_APPLY2_OMP(r_Size, r_Contig, tContig, scalar_t, r_, scalar_t, t, *r__data = *t_data % value;
                                        if (modulo_wrap(*r__data, value)) *r__data += value;, UNCERTAIN_TH_OMP_OVERHEAD_THRESHOLD);
#endif
    }
#else
    serial_path = 1;
#endif
  }
  if (serial_path) {
#if defined(TH_REAL_IS_FLOAT) || defined(TH_REAL_IS_DOUBLE)
    TH_TENSOR_APPLY2(scalar_t, r_, scalar_t, t, *r__data = (value == 0)? NAN : *t_data - value * floor(*t_data / value););
#else
    // There is no NAN for integers
    TH_TENSOR_APPLY2(scalar_t, r_, scalar_t, t, *r__data = *t_data % value;
                                          if (modulo_wrap(*r__data, value)) *r__data += value;);
#endif
  }
}

void THTensor_(bitand)(THTensor *r_, THTensor *t, scalar_t value)
{
#if defined(TH_REAL_IS_FLOAT) || defined(TH_REAL_IS_DOUBLE) || defined(TH_REAL_IS_HALF)
  (void)r_;
  (void)t;
  (void)value;
  return THError("bitand is only supported for integer type tensors");
#else
  THTensor_(resizeAs)(r_, t);
  int64_t r_Size = THTensor_(nElement)(r_);
  int r_Contig = THTensor_(isContiguous)(r_);
  int serial_path = 0;
  int tContig = THTensor_(isContiguous)(t);
  if (r_Contig && tContig) {
    scalar_t *tp = t->data<scalar_t>();
    scalar_t *rp = r_->data<scalar_t>();
    int64_t i;
    #pragma omp parallel for if(r_Size > TH_OMP_OVERHEAD_THRESHOLD * 100) private(i)
    for (i=0; i<r_Size; i++) {
      rp[i] = tp[i] & value;
    }
  } else {
#ifdef _OPENMP
    int inOMP = omp_in_parallel();
    if (inOMP) {
      serial_path = 1;
    } else {
      TH_TENSOR_APPLY2_OMP(r_Size, r_Contig, tContig, scalar_t, r_, scalar_t, t, *r__data = *t_data & value;, UNCERTAIN_TH_OMP_OVERHEAD_THRESHOLD);
    }
#else
    serial_path = 1;
#endif
  }
  if (serial_path) {
    TH_TENSOR_APPLY2(scalar_t, r_, scalar_t, t, *r__data = *t_data & value;);
  }
#endif
}

#endif /* TH_GENERIC_FILE */
