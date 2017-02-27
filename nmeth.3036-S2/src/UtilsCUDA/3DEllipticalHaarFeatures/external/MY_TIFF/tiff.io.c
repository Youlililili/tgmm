#ifdef _MSC_VER

#pragma warning( disable:4996 )

#ifndef __cplusplus
#define inline __inline
#endif

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

//  WINDOW pthreads "LIBRARY"

  //  Mutex macros

typedef SRWLOCK pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER RTL_SRWLOCK_INIT

#define pthread_mutex_lock(m)  AcquireSRWLockExclusive(m)

#define pthread_mutex_unlock(m) ReleaseSRWLockExclusive(m)

  //  Condition variable macros

typedef CONDITION_VARIABLE pthread_cond_t;

#define PTHREAD_COND_INITIALIZER RTL_CONDITION_VARIABLE_INIT

#define pthread_cond_signal(c) WakeConditionVariable(c)

#define pthread_cond_broadcast(c) WakeAllConditionVariable(c)

#define pthread_cond_wait(c,m) SleepConditionVariableSRW(c,m,INFINITE,0)

  //  Simple thread support

typedef struct
  { HANDLE *handle;
    void   *(*fct)(void *);
    void   *arg;
    void   *retval;
    int     id;
  } Mythread;

typedef Mythread *pthread_t;

static DWORD WINAPI MyStart(void *arg)
{ Mythread *tv = (Mythread *) arg;

  tv->retval = tv->fct(tv->arg);
  return (0);
}

static int pthread_create(pthread_t *thread, void *attr,
                          void *(*fct)(void *), void *arg)
{ Mythread *tv;
  if (attr != NULL)
    { fprintf(stderr,"Do not support thread attributes\n");
      exit (1);
    }
  tv = (Mythread *) malloc(sizeof(Mythread));
  if (tv == NULL)
    { fprintf(stderr,"pthread_create: Out of memory.\n");
      exit (1);
    };
  tv->fct    = fct;
  tv->arg    = arg;
  tv->handle = CreateThread(NULL,0,MyStart,tv,0,&tv->id);
  if (tv->handle == NULL)
    return (EAGAIN);
  else
    return (0);
}

static int pthread_join(pthread_t t, void **ret)
{ Mythread *tv = (Mythread *) t;

  WaitForSingleObject(tv->handle,INFINITE);
  if (ret != NULL)
    *ret = tv->retval;

  CloseHandle(tv->handle);
  free(tv);
  return (0);
}

typedef int pthread_id;

static pthread_id pthread_tag()
{ return (GetCurrentThreadId()); }

static int pthread_is_this(pthread_id id)
{ return (GetCurrentThreadId() == id); }

#else   //  Small extension to pthreads!

#include <pthread.h>

typedef pthread_t pthread_id;

#define pthread_tag() pthread_self()

static inline int pthread_is_this(pthread_id id)
{ return (pthread_equal(pthread_self(),id)); }

#endif

/*****************************************************************************************\
*                                                                                         *
*  Basic Tiff Reader and Writer (Tiff 6.0)                                                *
*    This program reads and writes .tif files without really understanding anything more  *
*    than that it is a header that has a linked lists of image file descriptors (ifd's)   *
*    that contain a collection of tags, one of which is an array pointing to blocks of    *
*    image data (TIFF_TILE_OFFSETS or TIFF_STRIP_OFFSETS) and another of which is an      *
*    array of byte counts for each block (TIFF_TILE_BYTE_COUNTS or TIFF_STRIP_BYTE_COUNTS,*
*    respectively).                                                                       *
*                                                                                         *
*    You can add, delete, and modify tags from each image as you wish.                    *
*                                                                                         *
*    Most importantly, we have "Tiff_Annotator" routines that allow one to efficiently    *
*      write  a TIFF_JF_ANO_BLOCK tag that points at a string buffer that is always       *
*      guaranteed to be at the end of the file so that changing it just is a matter of    *
*      rewriting the last bit of the file.                                                *
*                                                                                         *
*    The routines can also read and write .lsm files, consolidating them as proper tifs   *
*      once read and minimally reintroducing the essential "quirks" that an .lsm reader   *
*      would expect when writing back out (as a .lsm)                                     *
*                                                                                         *
*    The package handles a variant designed by us wherein offsets can be 64-bits and so   *
*      files larger than 2Gb are supported.  The image in an IFD may also be larger than  *
*      2Gb as well, but any individual dimension must fit in a 32-bit integer.            *
*                                                                                         *
*  Author:  Gene Myers                                                                    *
*  Date  :  January 2008                                                                  *
*                                                                                         *
*  (c) July 27, '09, Dr. Gene Myers and Howard Hughes Medical Institute                   *
*      Copyrighted as per the full copy in the associated 'README' file                   *
*                                                                                         *
\*****************************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <setjmp.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>

#ifdef _MSC_VER

#include <io.h>

#define stat            __stat64

#define close(x)        _close(x)
#define read(x,b,n)     _read(x,b,n)
#define lseek(x,o,w)    _lseek(x,o,w)
#define fseeko(f,o,p)   _fseeki64(f,o,p)
#define ftello(f)       _ftelli64(f)
#define fstat(f,b)      _fstat64(f,b)
#define ftruncate(f,o)  _chsize_s(f,o)
#define mktemp(e)       _mktemp(e)
#define fileno(e)       _fileno(e)

#else

#include <unistd.h>

#endif

#include "tiff.io.h"
#include "tiff.image.h"

typedef unsigned char      uint8;
typedef unsigned short     uint16;
typedef unsigned int       uint32;
typedef unsigned long long uint64;
typedef long long           int64;

//  These routines are privately shared between tiff.image and tiff.io, the
//    8qm5 suffix is to ensure the names do not conflict with those of a user
//    of the library

extern uint64 LZW_Counter_8qm5(FILE *input);
extern int    Write_IFD_Image_8qm5(Tiff_IFD *ifd, FILE *output);


/****************************************************************************************
 *                                                                                      *
 *  HEADERS AND INTERNAL DATA TYPES                                                     *
 *                                                                                      *
 ****************************************************************************************/

typedef struct
  { uint16  label;
    uint16  type;
    uint32  count;
    uint64  vector;   //  Word-boundary offset into array "values" of TIFD
  } Tif_Tag;

static int type_sizes[14] = //  Sizes in bytes of the tiff types (see tiff_io.h)
                            { 0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 8 };

#define POINTER(e)    ( & ((e)->vector) )

#define WORD_MULTIPLE(x) ((((x)-1)/8+1)*8)  // Will align values internally on quad-word boundaries

typedef struct
  { int     data_flip;  //  The image data needs to be endian-flipped when interpreted

    int     numtags;    //  Number of tags in the IFD
    int     maxtags;    //  Have room for this many (maxtags >= numtags)

    Tif_Tag *tags;      //  tags[0..maxtags] of tags the first numtags of which are being used

    int      vmax;      //  max size of the value block (veof <= vmax)
    int      veof;      //  current highwater mark of value block
    uint8   *values;    //  concatenation of all value vectors

    Tiff_Reader *source_ref;    //  source of this IFD, needed to fetch image data
    void        *data;          //  of if NULL, then base memory location of unpacked image data
  } TIFD;

typedef struct
  { int      flip;       //  bytes need to be flipped to get machine endian
    int      flag64;     //  offsets are 64 bits
    int      ifd_no;     //  # of next ifd to be read (start counting at 1)
    int      lsm;        //  this is an lsm file and you have to do hideous things to read it
    uint64   first_ifd;  //  offset of the first ifd in file
    uint64   ifd_offset; //  offset of ifd to be read next
    uint64   file_size;  //  size of the file in bytes (needed to guard against lsm counts)
    FILE    *input;
  } Treader;

typedef struct
  { int      flip;        //  bytes need to be flipped to get machine endian
    int      flag64;      //  offsets are 64 bits
    int      ifd_no;      //  # of next ifd to be written (start counting at 1)
    int      lsm;         //  this is an lsm file and you have to do hideous things as a result
    uint64   eof_offset;  //  current offset to end of what is written.
    uint64   ifd_linko;   //  offset of last ifd link
    uint32   ano_count;   //  length of annotation
    uint64   ano_linko;   //  offset of annotation link (0 => not present)
    char    *annotation;  //  memory block holding annotation
    FILE    *output;
  } Twriter;

typedef struct
  { int      flip;        //  bytes need to be flipped to get machine endian
    int      flag64;      //  offsets are 64 bits
    uint64   ano_cnto;    //  offset of annotation count
    uint64   ano_offset;  //  offset of annotation block
    uint32   ano_count;   //  length of annotation
    char    *annotation;  //  memory block holding annotation
    FILE    *inout;
  } Tannotator;


/****************************************************************************************
 *                                                                                      *
 *  LSM DECODING INFORMATION                                                            *
 *                                                                                      *
 ****************************************************************************************/

#define LSM_NO_CHANNELS           5  // offset to an int containing the # of channels
#define LSM_SCAN_TYPE            44  // offset to a short containing the scan type

// Offsets within the master block of offsets to secondary blocks and comments on how to
//   compute their sizes

#define LSM_VECTOR_OVERLAY       24  // 2nd-int
#define LSM_INPUT_LUT            25  // 1st-int
#define LSM_OUTPUT_LUT           26  // 1st-int
#define LSM_CHANNEL_COLORS       27  // 1st-int + LSM_NO_CHANNELS  ???
#define LSM_CHANNEL_DATA_TYPES   30  // 4*LSM_NO_CHANNELS
#define LSM_SCAN_INFORMATION     31  // 3rd-int + 12
#define LSM_KS_DATA              32  // not documented, zero  ???
#define LSM_TIME_STAMPS          33  // 1st-int
#define LSM_EVENT_LIST           34  // 1st-int
#define LSM_ROI                  35  // 2nd-int
#define LSM_BLEACH_ROI           36  // 2nd-int
#define LSM_NEXT_RECORDING       37  // another entire tiff, geez, zero
#define LSM_MEAN_OF_ROIS_OVERLAY 45  // 2nd-int (iff SCAN_TYPE = 5), 0 otherwise
#define LSM_TOPO_ISOLINE_OVERLAY 46  // 2nd-int
#define LSM_TOPO_PROFILE_OVERLAY 47  // 2nd-int
#define LSM_LINE_SCAN_OVERLAY    48  // 2nd-int
#define LSM_CHANNEL_WAVELENGTH   50  // 16*1st-int + 4
#define LSM_CHANNEL_FACTORS      51  // 24*LSM_NO_CHANNELS
#define LSM_UNMIX_PARAMETERS     54  // not documented, zero  ???
#define LSM_ASCII_INFORMATION    56  // 1st-int  (reverse engineered)
#define LSM_ASCII_SIGNATURE      57  // 1st-int  (reverse engineered)

static int lsm_offset_list[] = { LSM_VECTOR_OVERLAY, LSM_INPUT_LUT, LSM_OUTPUT_LUT,
                                 LSM_CHANNEL_COLORS, LSM_CHANNEL_DATA_TYPES, LSM_SCAN_INFORMATION,
                                 LSM_KS_DATA, LSM_TIME_STAMPS, LSM_EVENT_LIST, LSM_ROI,
                                 LSM_BLEACH_ROI, LSM_NEXT_RECORDING, LSM_MEAN_OF_ROIS_OVERLAY,
                                 LSM_TOPO_ISOLINE_OVERLAY, LSM_TOPO_PROFILE_OVERLAY,
                                 LSM_LINE_SCAN_OVERLAY, LSM_CHANNEL_WAVELENGTH,
                                 LSM_CHANNEL_FACTORS, LSM_UNMIX_PARAMETERS, LSM_ASCII_INFORMATION,
                                 LSM_ASCII_SIGNATURE, 0
                               };

static int lsm_1st_size[] = { LSM_INPUT_LUT, LSM_OUTPUT_LUT, LSM_TIME_STAMPS, LSM_EVENT_LIST,
                              LSM_ASCII_INFORMATION, LSM_ASCII_SIGNATURE, 0 };

static int lsm_2nd_size[] = { LSM_VECTOR_OVERLAY, LSM_ROI, LSM_BLEACH_ROI,
                              LSM_TOPO_ISOLINE_OVERLAY, LSM_TOPO_PROFILE_OVERLAY,
                              LSM_LINE_SCAN_OVERLAY, 0
                            };

static int lsm_zero_size[] = { LSM_KS_DATA, LSM_NEXT_RECORDING, LSM_UNMIX_PARAMETERS, 0 };


/****************************************************************************************
 *                                                                                      *
 *  BASIC OBJECT MEMORY MANAGEMENT                                                      *
 *                                                                                      *
 ****************************************************************************************/

  //  If an Tiff routine encounters an exception it trys to grab the "error resource" with
  //    Grab_Message_8qm5 and if successfull then sets the global strings Tiff_Estring/Esource
  //    before returning NULL.  A user can subsequently get these strings with Tiff_Error_String()
  //    and Tiff_Error_Source() and release the "error resource" with a call to
  //    Tiff_Error_Release().

static pthread_mutex_t Tiff_Err_Mutex = PTHREAD_MUTEX_INITIALIZER;

static int        Tiff_Error = 0;
static char       Tiff_Estring[1000];
static char       Tiff_Esource[100];
static pthread_id Tiff_Ethread;

int Grab_Message_8qm5()
{ int ret;
  pthread_mutex_lock(&Tiff_Err_Mutex);
  if (Tiff_Error)
    ret = 0;
  else
    { Tiff_Error   = 1;
      Tiff_Ethread = pthread_tag();
      ret = 1;
    }
  pthread_mutex_unlock(&Tiff_Err_Mutex);
  return (ret);
}

static int my_message()
{ int mine;
  pthread_mutex_lock(&Tiff_Err_Mutex);
  mine = (Tiff_Error && pthread_is_this(Tiff_Ethread));
  pthread_mutex_unlock(&Tiff_Err_Mutex);
  return (mine);
}

char *Tiff_Error_Source()
{ char *ret;
  pthread_mutex_lock(&Tiff_Err_Mutex);
  if (Tiff_Error && pthread_is_this(Tiff_Ethread))
    ret = Tiff_Esource;
  else
    ret = NULL;
  pthread_mutex_unlock(&Tiff_Err_Mutex);
  return (ret);
}

char *Tiff_Error_String()
{ char *ret;
  pthread_mutex_lock(&Tiff_Err_Mutex);
  if (Tiff_Error && pthread_is_this(Tiff_Ethread))
    ret = Tiff_Estring;
  else
    ret = NULL;
  pthread_mutex_unlock(&Tiff_Err_Mutex);
  return (ret);
}

void Tiff_Error_Release()
{ pthread_mutex_lock(&Tiff_Err_Mutex);
  if (Tiff_Error && pthread_is_this(Tiff_Ethread))
    Tiff_Error = 0;
  pthread_mutex_unlock(&Tiff_Err_Mutex);
}

static void *Guarded_Realloc(void *p, size_t size, char *routine)
{ (void) routine;
  p = realloc(p,size);
  if (p == NULL)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Out of memory");
    }
  return (p);
}


typedef struct __Treader
  { struct __Treader *next;
    struct __Treader *prev;
    int               refcnt;
    Treader           treader;
  } _Treader;

static _Treader *Free_Treader_List = NULL;
static _Treader *Use_Treader_List  = NULL;

static pthread_mutex_t Treader_Mutex = PTHREAD_MUTEX_INITIALIZER;

static int Treader_Offset = sizeof(_Treader)-sizeof(Treader);
static int Treader_Inuse  = 0;

int Tiff_Reader_Refcount(Tiff_Reader *tiff_reader)
{ _Treader *object = (_Treader *) (((char *) tiff_reader) - Treader_Offset);
  return (object->refcnt);
}

static inline void kill_treader(Treader *treader);

static inline Treader *new_treader(char *routine)
{ _Treader *object;
  Treader  *treader;

  pthread_mutex_lock(&Treader_Mutex);
  if (Free_Treader_List == NULL)
    { object = (_Treader *) Guarded_Realloc(NULL,sizeof(_Treader),routine);
      if (object == NULL) return (NULL);
      treader = &(object->treader);
    }
  else
    { object = Free_Treader_List;
      Free_Treader_List = object->next;
      treader = &(object->treader);
    }
  Treader_Inuse += 1;
  object->refcnt = 1;
  if (Use_Treader_List != NULL)
    Use_Treader_List->prev = object;
  object->next = Use_Treader_List;
  object->prev = NULL;
  Use_Treader_List = object;
  pthread_mutex_unlock(&Treader_Mutex);
  return (treader);
}

static inline Treader *copy_treader(Treader *treader)
{ Treader *copy = new_treader("Copy_Tiff_Reader");
  *copy = *treader;
  return (copy);
}

Tiff_Reader *Copy_Tiff_Reader(Tiff_Reader *tiff_reader)
{ return ((Tiff_Reader *) copy_treader(((Treader *) tiff_reader))); }

Tiff_Reader *Pack_Tiff_Reader(Tiff_Reader *tiff_reader)
{ return (tiff_reader); }

Tiff_Reader *Inc_Tiff_Reader(Tiff_Reader *tiff_reader)
{ _Treader *object  = (_Treader *) (((char *) tiff_reader) - Treader_Offset);
  pthread_mutex_lock(&Treader_Mutex);
  object->refcnt += 1;
  pthread_mutex_unlock(&Treader_Mutex);
  return (tiff_reader);
}

static inline void free_treader(Treader *treader)
{ _Treader *object  = (_Treader *) (((char *) treader) - Treader_Offset);
  pthread_mutex_lock(&Treader_Mutex);
  if (--object->refcnt > 0)
    { pthread_mutex_unlock(&Treader_Mutex);
      return;
    }
  if (object->refcnt < 0)
    fprintf(stderr,"Warning: Freeing previously released Tiff_Reader\n");
  if (object->prev != NULL)
    object->prev->next = object->next;
  else
    Use_Treader_List = object->next;
  if (object->next != NULL)
    object->next->prev = object->prev;
  object->next = Free_Treader_List;
  Free_Treader_List = object;
  Treader_Inuse -= 1;
  pthread_mutex_unlock(&Treader_Mutex);
}

static inline void kill_treader(Treader *treader)
{ _Treader *object  = (_Treader *) (((char *) treader) - Treader_Offset);
  pthread_mutex_lock(&Treader_Mutex);
  if (--object->refcnt > 0)
    { pthread_mutex_unlock(&Treader_Mutex);
      return;
    }
  if (object->refcnt < 0)
    fprintf(stderr,"Warning: Killing previously released Tiff_Reader\n");
  if (object->prev != NULL)
    object->prev->next = object->next;
  else
    Use_Treader_List = object->next;
  if (object->next != NULL)
    object->next->prev = object->prev;
  Treader_Inuse -= 1;
  pthread_mutex_unlock(&Treader_Mutex);
  free(((char *) treader) - Treader_Offset);
}

static inline void reset_treader()
{ _Treader *object;
  pthread_mutex_lock(&Treader_Mutex);
  while (Free_Treader_List != NULL)
    { object = Free_Treader_List;
      Free_Treader_List = object->next;
      free(object);
    }
  pthread_mutex_unlock(&Treader_Mutex);
}

void Reset_Tiff_Reader()
{ reset_treader(); }

int Tiff_Reader_Usage()
{ return (Treader_Inuse); }

void Tiff_Reader_List(void (*handler)(Tiff_Reader *))
{ _Treader *a, *b;
  for (a = Use_Treader_List; a != NULL; a = b)
    { b = a->next;
      handler((Tiff_Reader *) &(a->treader));
    }
}

void Free_Tiff_Reader(Tiff_Reader *tif)
{ Treader *rtif = (Treader *) tif;
  if (Tiff_Reader_Refcount(tif) == 1)
    fclose(rtif->input);
  free_treader(rtif);
}

void Kill_Tiff_Reader(Tiff_Reader *tif)
{ Treader *rtif = (Treader *) tif;
  if (Tiff_Reader_Refcount(tif) == 1)
    fclose(rtif->input);
  kill_treader(rtif);
}

static inline int twriter_asize(Twriter *tif)
{ return ((int) tif->ano_count); }


typedef struct __Twriter
  { struct __Twriter *next;
    struct __Twriter *prev;
    int               refcnt;
    int               asize;
    Twriter           twriter;
  } _Twriter;

static _Twriter *Free_Twriter_List = NULL;
static _Twriter *Use_Twriter_List  = NULL;

static pthread_mutex_t Twriter_Mutex = PTHREAD_MUTEX_INITIALIZER;

static int Twriter_Offset = sizeof(_Twriter)-sizeof(Twriter);
static int Twriter_Inuse  = 0;

int Tiff_Writer_Refcount(Tiff_Writer *tiff_writer)
{ _Twriter *object = (_Twriter *) (((char *) tiff_writer) - Twriter_Offset);
  return (object->refcnt);
}

static inline int allocate_twriter_annotation(Twriter *twriter, int asize, char *routine)
{ _Twriter *object = (_Twriter *) (((char *) twriter) - Twriter_Offset);
  if (object->asize < asize)
    { void *x = Guarded_Realloc(twriter->annotation,(size_t) asize,routine);
      if (x == NULL) return (1);
      twriter->annotation = x;
      object->asize = asize;
    }
  return (0);
}

static inline int sizeof_twriter_annotation(Twriter *twriter)
{ _Twriter *object = (_Twriter *) (((char *) twriter) - Twriter_Offset);
  return (object->asize);
}

static inline void kill_twriter(Twriter *twriter);

static inline Twriter *new_twriter(int asize, char *routine)
{ _Twriter *object;
  Twriter  *twriter;

  pthread_mutex_lock(&Twriter_Mutex);
  if (Free_Twriter_List == NULL)
    { object = (_Twriter *) Guarded_Realloc(NULL,sizeof(_Twriter),routine);
      if (object == NULL) return (NULL);
      twriter = &(object->twriter);
      object->asize = 0;
      twriter->annotation = NULL;
    }
  else
    { object = Free_Twriter_List;
      Free_Twriter_List = object->next;
      twriter = &(object->twriter);
    }
  Twriter_Inuse += 1;
  object->refcnt = 1;
  if (Use_Twriter_List != NULL)
    Use_Twriter_List->prev = object;
  object->next = Use_Twriter_List;
  object->prev = NULL;
  Use_Twriter_List = object;
  pthread_mutex_unlock(&Twriter_Mutex);
  if (allocate_twriter_annotation(twriter,asize,routine))
    { kill_twriter(twriter);
      return (NULL);
    }
  return (twriter);
}

static inline Twriter *copy_twriter(Twriter *twriter)
{ Twriter *copy = new_twriter(twriter_asize(twriter),"Copy_Tiff_Writer");
  void *_annotation = copy->annotation;
  *copy = *twriter;
  copy->annotation = _annotation;
  if (twriter->annotation != NULL)
    memcpy(copy->annotation,twriter->annotation,(size_t) twriter_asize(twriter));
  return (copy);
}

Tiff_Writer *Copy_Tiff_Writer(Tiff_Writer *tiff_writer)
{ return ((Tiff_Writer *) copy_twriter(((Twriter *) tiff_writer))); }

static inline int pack_twriter(Twriter *twriter)
{ _Twriter *object  = (_Twriter *) (((char *) twriter) - Twriter_Offset);
  if (object->asize > twriter_asize(twriter))
    { int ns = twriter_asize(twriter);
      if (ns != 0)
        { void *x = Guarded_Realloc(twriter->annotation,(size_t) ns,"Pack_Twriter");
          if (x == NULL) return (1);
          twriter->annotation = x;
        }
      else
        { free(twriter->annotation);
          twriter->annotation = NULL;
        }
      object->asize = ns;
    }
  return (0);
}

Tiff_Writer *Pack_Tiff_Writer(Tiff_Writer *tiff_writer)
{ if (pack_twriter(((Twriter *) tiff_writer))) return (NULL);
  return (tiff_writer);
}

Tiff_Writer *Inc_Tiff_Writer(Tiff_Writer *tiff_writer)
{ _Twriter *object  = (_Twriter *) (((char *) tiff_writer) - Twriter_Offset);
  pthread_mutex_lock(&Twriter_Mutex);
  object->refcnt += 1;
  pthread_mutex_unlock(&Twriter_Mutex);
  return (tiff_writer);
}

static inline void free_twriter(Twriter *twriter)
{ _Twriter *object  = (_Twriter *) (((char *) twriter) - Twriter_Offset);
  pthread_mutex_lock(&Twriter_Mutex);
  if (--object->refcnt > 0)
    { pthread_mutex_unlock(&Twriter_Mutex);
      return;
    }
  if (object->refcnt < 0)
    fprintf(stderr,"Warning: Freeing previously released Tiff_Writer\n");
  if (object->prev != NULL)
    object->prev->next = object->next;
  else
    Use_Twriter_List = object->next;
  if (object->next != NULL)
    object->next->prev = object->prev;
  object->next = Free_Twriter_List;
  Free_Twriter_List = object;
  Twriter_Inuse -= 1;
  pthread_mutex_unlock(&Twriter_Mutex);
}

static inline void kill_twriter(Twriter *twriter)
{ _Twriter *object  = (_Twriter *) (((char *) twriter) - Twriter_Offset);
  pthread_mutex_lock(&Twriter_Mutex);
  if (--object->refcnt > 0)
    { pthread_mutex_unlock(&Twriter_Mutex);
      return;
    }
  if (object->refcnt < 0)
    fprintf(stderr,"Warning: Killing previously released Tiff_Writer\n");
  if (object->prev != NULL)
    object->prev->next = object->next;
  else
    Use_Twriter_List = object->next;
  if (object->next != NULL)
    object->next->prev = object->prev;
  Twriter_Inuse -= 1;
  pthread_mutex_unlock(&Twriter_Mutex);
  if (twriter->annotation != NULL)
    free(twriter->annotation);
  free(((char *) twriter) - Twriter_Offset);
}

static inline void reset_twriter()
{ _Twriter *object;
  Twriter  *twriter;
  pthread_mutex_lock(&Twriter_Mutex);
  while (Free_Twriter_List != NULL)
    { object = Free_Twriter_List;
      Free_Twriter_List = object->next;
      twriter = &(object->twriter);
      if (twriter->annotation != NULL)
        free(twriter->annotation);
      free(object);
    }
  pthread_mutex_unlock(&Twriter_Mutex);
}

void Reset_Tiff_Writer()
{ reset_twriter(); }

int Tiff_Writer_Usage()
{ return (Twriter_Inuse); }

void Tiff_Writer_List(void (*handler)(Tiff_Writer *))
{ _Twriter *a, *b;
  for (a = Use_Twriter_List; a != NULL; a = b)
    { b = a->next;
      handler((Tiff_Writer *) &(a->twriter));
    }
}

static void close_tiff_writer(Twriter *tif);

void Free_Tiff_Writer(Tiff_Writer *tif)
{ Twriter *wtif = (Twriter *) tif;
  if (Tiff_Writer_Refcount(tif) == 1)
    close_tiff_writer(wtif);
  free_twriter(wtif);
}

void Kill_Tiff_Writer(Tiff_Writer *tif)
{ Twriter *wtif = (Twriter *) tif;
  if (Tiff_Writer_Refcount(tif) == 1)
    close_tiff_writer(wtif);
  kill_twriter(wtif);
}

static inline int tannotator_asize(Tannotator *tif)
{ return ((int) tif->ano_count); }


typedef struct __Tannotator
  { struct __Tannotator *next;
    struct __Tannotator *prev;
    int                  refcnt;
    int                  asize;
    Tannotator           tannotator;
  } _Tannotator;

static _Tannotator *Free_Tannotator_List = NULL;
static _Tannotator *Use_Tannotator_List  = NULL;

static pthread_mutex_t Tannotator_Mutex = PTHREAD_MUTEX_INITIALIZER;

static int Tannotator_Offset = sizeof(_Tannotator)-sizeof(Tannotator);
static int Tannotator_Inuse  = 0;

int Tiff_Annotator_Refcount(Tiff_Annotator *tiff_annotator)
{ _Tannotator *object = (_Tannotator *) (((char *) tiff_annotator) - Tannotator_Offset);
  return (object->refcnt);
}

static inline int allocate_tannotator_annotation(Tannotator *tannotator, int asize, char *routine)
{ _Tannotator *object = (_Tannotator *) (((char *) tannotator) - Tannotator_Offset);
  if (object->asize < asize)
    { void *x = Guarded_Realloc(tannotator->annotation,(size_t) asize,routine);
      if (x == NULL) return (1);
      tannotator->annotation = x;
      object->asize = asize;
    }
  return (0);
}

static inline int sizeof_tannotator_annotation(Tannotator *tannotator)
{ _Tannotator *object = (_Tannotator *) (((char *) tannotator) - Tannotator_Offset);
  return (object->asize);
}

static inline void kill_tannotator(Tannotator *tannotator);

static inline Tannotator *new_tannotator(int asize, char *routine)
{ _Tannotator *object;
  Tannotator  *tannotator;

  pthread_mutex_lock(&Tannotator_Mutex);
  if (Free_Tannotator_List == NULL)
    { object = (_Tannotator *) Guarded_Realloc(NULL,sizeof(_Tannotator),routine);
      if (object == NULL) return (NULL);
      tannotator = &(object->tannotator);
      object->asize = 0;
      tannotator->annotation = NULL;
    }
  else
    { object = Free_Tannotator_List;
      Free_Tannotator_List = object->next;
      tannotator = &(object->tannotator);
    }
  Tannotator_Inuse += 1;
  object->refcnt = 1;
  if (Use_Tannotator_List != NULL)
    Use_Tannotator_List->prev = object;
  object->next = Use_Tannotator_List;
  object->prev = NULL;
  Use_Tannotator_List = object;
  pthread_mutex_unlock(&Tannotator_Mutex);
  if (allocate_tannotator_annotation(tannotator,asize,routine))
    { kill_tannotator(tannotator);
      return (NULL);
    }
  return (tannotator);
}

static inline Tannotator *copy_tannotator(Tannotator *tannotator)
{ Tannotator *copy = new_tannotator(tannotator_asize(tannotator),"Copy_Tiff_Annotator");
  void *_annotation = copy->annotation;
  *copy = *tannotator;
  copy->annotation = _annotation;
  if (tannotator->annotation != NULL)
    memcpy(copy->annotation,tannotator->annotation,(size_t) tannotator_asize(tannotator));
  return (copy);
}

Tiff_Annotator *Copy_Tiff_Annotator(Tiff_Annotator *tiff_annotator)
{ return ((Tiff_Annotator *) copy_tannotator(((Tannotator *) tiff_annotator))); }

static inline int pack_tannotator(Tannotator *tannotator)
{ _Tannotator *object  = (_Tannotator *) (((char *) tannotator) - Tannotator_Offset);
  if (object->asize > tannotator_asize(tannotator))
    { int ns = tannotator_asize(tannotator);
      if (ns != 0)
        { void *x = Guarded_Realloc(tannotator->annotation,(size_t) ns,"Pack_Tannotator");
          if (x == NULL) return (1);
          tannotator->annotation = x;
        }
      else
        { free(tannotator->annotation);
          tannotator->annotation = NULL;
        }
      object->asize = ns;
    }
  return (0);
}

Tiff_Annotator *Pack_Tiff_Annotator(Tiff_Annotator *tiff_annotator)
{ if (pack_tannotator(((Tannotator *) tiff_annotator))) return (NULL);
  return (tiff_annotator);
}

Tiff_Annotator *Inc_Tiff_Annotator(Tiff_Annotator *tiff_annotator)
{ _Tannotator *object  = (_Tannotator *) (((char *) tiff_annotator) - Tannotator_Offset);
  pthread_mutex_lock(&Tannotator_Mutex);
  object->refcnt += 1;
  pthread_mutex_unlock(&Tannotator_Mutex);
  return (tiff_annotator);
}

static inline void free_tannotator(Tannotator *tannotator)
{ _Tannotator *object  = (_Tannotator *) (((char *) tannotator) - Tannotator_Offset);
  pthread_mutex_lock(&Tannotator_Mutex);
  if (--object->refcnt > 0)
    { pthread_mutex_unlock(&Tannotator_Mutex);
      return;
    }
  if (object->refcnt < 0)
    fprintf(stderr,"Warning: Freeing previously released Tiff_Annotator\n");
  if (object->prev != NULL)
    object->prev->next = object->next;
  else
    Use_Tannotator_List = object->next;
  if (object->next != NULL)
    object->next->prev = object->prev;
  object->next = Free_Tannotator_List;
  Free_Tannotator_List = object;
  Tannotator_Inuse -= 1;
  pthread_mutex_unlock(&Tannotator_Mutex);
}

static inline void kill_tannotator(Tannotator *tannotator)
{ _Tannotator *object  = (_Tannotator *) (((char *) tannotator) - Tannotator_Offset);
  pthread_mutex_lock(&Tannotator_Mutex);
  if (--object->refcnt > 0)
    { pthread_mutex_unlock(&Tannotator_Mutex);
      return;
    }
  if (object->refcnt < 0)
    fprintf(stderr,"Warning: Killing previously released Tiff_Annotator\n");
  if (object->prev != NULL)
    object->prev->next = object->next;
  else
    Use_Tannotator_List = object->next;
  if (object->next != NULL)
    object->next->prev = object->prev;
  Tannotator_Inuse -= 1;
  pthread_mutex_unlock(&Tannotator_Mutex);
  if (tannotator->annotation != NULL)
    free(tannotator->annotation);
  free(((char *) tannotator) - Tannotator_Offset);
}

static inline void reset_tannotator()
{ _Tannotator *object;
  Tannotator  *tannotator;
  pthread_mutex_lock(&Tannotator_Mutex);
  while (Free_Tannotator_List != NULL)
    { object = Free_Tannotator_List;
      Free_Tannotator_List = object->next;
      tannotator = &(object->tannotator);
      if (tannotator->annotation != NULL)
        free(tannotator->annotation);
      free(object);
    }
  pthread_mutex_unlock(&Tannotator_Mutex);
}

void Reset_Tiff_Annotator()
{ reset_tannotator(); }

int Tiff_Annotator_Usage()
{ return (Tannotator_Inuse); }

void Tiff_Annotator_List(void (*handler)(Tiff_Annotator *))
{ _Tannotator *a, *b;
  for (a = Use_Tannotator_List; a != NULL; a = b)
    { b = a->next;
      handler((Tiff_Annotator *) &(a->tannotator));
    }
}

static void close_tiff_annotator(Tannotator *tif);

void Free_Tiff_Annotator(Tiff_Annotator *tif)
{ Tannotator *atif = (Tannotator *) tif;
  if (Tiff_Annotator_Refcount(tif) == 1)
    close_tiff_annotator(atif);
  free_tannotator(atif);
}

void Kill_Tiff_Annotator(Tiff_Annotator *tif)
{ Tannotator *atif = (Tannotator *) tif;
  if (Tiff_Annotator_Refcount(tif) == 1)
    close_tiff_annotator(atif);
  kill_tannotator(atif);
}

static inline int tifd_tsize(TIFD *tifd)
{ return (((int) sizeof(Tif_Tag))*tifd->maxtags); }

static inline int tifd_vsize(TIFD *tifd)
{ return (tifd->vmax); }


typedef struct __TIFD
  { struct __TIFD *next;
    struct __TIFD *prev;
    int            refcnt;
    int            tsize;
    int            vsize;
    TIFD           tifd;
  } _TIFD;

static _TIFD *Free_TIFD_List = NULL;
static _TIFD *Use_TIFD_List  = NULL;

static pthread_mutex_t TIFD_Mutex = PTHREAD_MUTEX_INITIALIZER;

static int TIFD_Offset = sizeof(_TIFD)-sizeof(TIFD);
static int TIFD_Inuse  = 0;

int Tiff_IFD_Refcount(Tiff_IFD *tiff_ifd)
{ _TIFD *object = (_TIFD *) (((char *) tiff_ifd) - TIFD_Offset);
  return (object->refcnt);
}

static inline int allocate_tifd_tags(TIFD *tifd, int tsize, char *routine)
{ _TIFD *object = (_TIFD *) (((char *) tifd) - TIFD_Offset);
  if (object->tsize < tsize)
    { void *x = Guarded_Realloc(tifd->tags,(size_t) tsize,routine);
      if (x == NULL) return (1);
      tifd->tags = x;
      object->tsize = tsize;
    }
  return (0);
}

static inline int sizeof_tifd_tags(TIFD *tifd)
{ _TIFD *object = (_TIFD *) (((char *) tifd) - TIFD_Offset);
  return (object->tsize);
}

static inline int allocate_tifd_values(TIFD *tifd, int vsize, char *routine)
{ _TIFD *object = (_TIFD *) (((char *) tifd) - TIFD_Offset);
  if (object->vsize < vsize)
    { void *x = Guarded_Realloc(tifd->values,(size_t) vsize,routine);
      if (x == NULL) return (1);
      tifd->values = x;
      object->vsize = vsize;
    }
  return (0);
}

static inline int sizeof_tifd_values(TIFD *tifd)
{ _TIFD *object = (_TIFD *) (((char *) tifd) - TIFD_Offset);
  return (object->vsize);
}

static inline void kill_tifd(TIFD *tifd);

static inline TIFD *new_tifd(int tsize, int vsize, char *routine)
{ _TIFD *object;
  TIFD  *tifd;

  pthread_mutex_lock(&TIFD_Mutex);
  if (Free_TIFD_List == NULL)
    { object = (_TIFD *) Guarded_Realloc(NULL,sizeof(_TIFD),routine);
      if (object == NULL) return (NULL);
      tifd = &(object->tifd);
      object->tsize = 0;
      tifd->tags = NULL;
      object->vsize = 0;
      tifd->values = NULL;
    }
  else
    { object = Free_TIFD_List;
      Free_TIFD_List = object->next;
      tifd = &(object->tifd);
    }
  TIFD_Inuse += 1;
  object->refcnt = 1;
  if (Use_TIFD_List != NULL)
    Use_TIFD_List->prev = object;
  object->next = Use_TIFD_List;
  object->prev = NULL;
  Use_TIFD_List = object;
  pthread_mutex_unlock(&TIFD_Mutex);
  tifd->source_ref = NULL;
  if (allocate_tifd_tags(tifd,tsize,routine))
    { kill_tifd(tifd);
      return (NULL);
    }
  if (allocate_tifd_values(tifd,vsize,routine))
    { kill_tifd(tifd);
      return (NULL);
    }
  return (tifd);
}

static inline TIFD *copy_tifd(TIFD *tifd)
{ TIFD *copy = new_tifd(tifd_tsize(tifd),tifd_vsize(tifd),"Copy_Tiff_IFD");
  void *_tags = copy->tags;
  void *_values = copy->values;
  *copy = *tifd;
  copy->tags = _tags;
  if (tifd->tags != NULL)
    memcpy(copy->tags,tifd->tags,(size_t) tifd_tsize(tifd));
  copy->values = _values;
  if (tifd->values != NULL)
    memcpy(copy->values,tifd->values,(size_t) tifd_vsize(tifd));
  if (tifd->source_ref != NULL)
    Inc_Tiff_Reader(tifd->source_ref);
  return (copy);
}

Tiff_IFD *Copy_Tiff_IFD(Tiff_IFD *tiff_ifd)
{ return ((Tiff_IFD *) copy_tifd(((TIFD *) tiff_ifd))); }

static inline int pack_tifd(TIFD *tifd)
{ _TIFD *object  = (_TIFD *) (((char *) tifd) - TIFD_Offset);
  if (object->tsize > tifd_tsize(tifd))
    { int ns = tifd_tsize(tifd);
      if (ns != 0)
        { void *x = Guarded_Realloc(tifd->tags,(size_t) ns,"Pack_TIFD");
          if (x == NULL) return (1);
          tifd->tags = x;
        }
      else
        { free(tifd->tags);
          tifd->tags = NULL;
        }
      object->tsize = ns;
    }
  if (object->vsize > tifd_vsize(tifd))
    { int ns = tifd_vsize(tifd);
      if (ns != 0)
        { void *x = Guarded_Realloc(tifd->values,(size_t) ns,"Pack_TIFD");
          if (x == NULL) return (1);
          tifd->values = x;
        }
      else
        { free(tifd->values);
          tifd->values = NULL;
        }
      object->vsize = ns;
    }
  return (0);
}

Tiff_IFD *Pack_Tiff_IFD(Tiff_IFD *tiff_ifd)
{ if (pack_tifd(((TIFD *) tiff_ifd))) return (NULL);
  return (tiff_ifd);
}

Tiff_IFD *Inc_Tiff_IFD(Tiff_IFD *tiff_ifd)
{ _TIFD *object  = (_TIFD *) (((char *) tiff_ifd) - TIFD_Offset);
  pthread_mutex_lock(&TIFD_Mutex);
  object->refcnt += 1;
  pthread_mutex_unlock(&TIFD_Mutex);
  return (tiff_ifd);
}

static inline void free_tifd(TIFD *tifd)
{ _TIFD *object  = (_TIFD *) (((char *) tifd) - TIFD_Offset);
  pthread_mutex_lock(&TIFD_Mutex);
  if (--object->refcnt > 0)
    { pthread_mutex_unlock(&TIFD_Mutex);
      return;
    }
  if (object->refcnt < 0)
    fprintf(stderr,"Warning: Freeing previously released Tiff_IFD\n");
  if (object->prev != NULL)
    object->prev->next = object->next;
  else
    Use_TIFD_List = object->next;
  if (object->next != NULL)
    object->next->prev = object->prev;
  object->next = Free_TIFD_List;
  Free_TIFD_List = object;
  TIFD_Inuse -= 1;
  pthread_mutex_unlock(&TIFD_Mutex);
  if (tifd->source_ref != NULL)
    { Free_Tiff_Reader(tifd->source_ref);
      tifd->source_ref = NULL;
    }
}

void Free_Tiff_IFD(Tiff_IFD *tiff_ifd)
{ free_tifd(((TIFD *) tiff_ifd)); }

static inline void kill_tifd(TIFD *tifd)
{ _TIFD *object  = (_TIFD *) (((char *) tifd) - TIFD_Offset);
  pthread_mutex_lock(&TIFD_Mutex);
  if (--object->refcnt > 0)
    { pthread_mutex_unlock(&TIFD_Mutex);
      return;
    }
  if (object->refcnt < 0)
    fprintf(stderr,"Warning: Killing previously released Tiff_IFD\n");
  if (object->prev != NULL)
    object->prev->next = object->next;
  else
    Use_TIFD_List = object->next;
  if (object->next != NULL)
    object->next->prev = object->prev;
  TIFD_Inuse -= 1;
  pthread_mutex_unlock(&TIFD_Mutex);
  if (tifd->source_ref != NULL)
    Kill_Tiff_Reader(tifd->source_ref);
  if (tifd->values != NULL)
    free(tifd->values);
  if (tifd->tags != NULL)
    free(tifd->tags);
  free(((char *) tifd) - TIFD_Offset);
}

void Kill_Tiff_IFD(Tiff_IFD *tiff_ifd)
{ kill_tifd(((TIFD *) tiff_ifd)); }

static inline void reset_tifd()
{ _TIFD *object;
  TIFD  *tifd;
  pthread_mutex_lock(&TIFD_Mutex);
  while (Free_TIFD_List != NULL)
    { object = Free_TIFD_List;
      Free_TIFD_List = object->next;
      tifd = &(object->tifd);
      if (tifd->values != NULL)
        free(tifd->values);
      if (tifd->tags != NULL)
        free(tifd->tags);
      free(object);
    }
  pthread_mutex_unlock(&TIFD_Mutex);
}

void Reset_Tiff_IFD()
{ reset_tifd(); }

int Tiff_IFD_Usage()
{ return (TIFD_Inuse); }

void Tiff_IFD_List(void (*handler)(Tiff_IFD *))
{ _TIFD *a, *b;
  for (a = Use_TIFD_List; a != NULL; a = b)
    { b = a->next;
      handler((Tiff_IFD *) &(a->tifd));
    }
}


/****************************************************************************************
 *                                                                                      *
 *  IFD PRINTING ROUTINE                                                                *
 *                                                                                      *
 ****************************************************************************************/

static char *tiff_label[] =
      { "NEW_SUB_FILE_TYPE",
        "SUB_FILE_TYPE",
        "IMAGE_WIDTH",
        "IMAGE_LENGTH",
        "BITS_PER_SAMPLE",
        "COMPRESSION",
        "???",
        "???",
        "PHOTOMETRIC_INTERPRETATION",
        "THRESHHOLDING",
        "CELL_WIDTH",
        "CELL_LENGTH",
        "FILL_ORDER",
        "???",
        "???",
        "DOCUMENT_NAME",
        "IMAGE_DESCRIPTION",
        "MAKE",
        "MODEL",
        "STRIP_OFFSETS",
        "ORIENTATION",
        "???",
        "???",
        "SAMPLES_PER_PIXEL",
        "ROWS_PER_STRIP",
        "STRIP_BYTE_COUNTS",
        "MIN_SAMPLE_VALUE",
        "MAX_SAMPLE_VALUE",
        "X_RESOLUTION",
        "Y_RESOLUTION",
        "PLANAR_CONFIGURATION",
        "PAGE_NAME",
        "X_POSITION",
        "Y_POSITION",
        "FREE_OFFSETS",
        "FREE_BYTE_COUNTS",
        "GRAY_RESPONSE_UNIT",
        "GRAY_RESPONSE_CURVE",
        "T4_OPTIONS",
        "T6_OPTIONS",
        "???",
        "???",
        "RESOLUTION_UNIT",
        "PAGE_NUMBER",
        "???",
        "???",
        "???",
        "TRANSFER_FUNCTION",
        "???",
        "???",
        "???",
        "SOFTWARE",
        "DATE_TIME",
        "???",
        "???",
        "???",
        "???",
        "???",
        "???",
        "???",
        "???",
        "ARTIST",
        "HOST_COMPUTER",
        "PREDICTOR",
        "WHITE_POINT",
        "PRIMARY_CHROMATICITIES",
        "COLOR_MAP",
        "HALFTONE_HINTS",
        "TILE_WIDTH",
        "TILE_LENGTH",
        "TILE_OFFSETS",
        "TILE_BYTE_COUNTS",
        "???",
        "???",
        "???",
        "???",
        "???",
        "???",
        "INK_SET",
        "INK_NAMES",
        "NUMBER_OF_INKS",
        "???",
        "DOT_RANGE",
        "TARGET_PRINTER",
        "EXTRA_SAMPLES",
        "SAMPLE_FORMAT",
        "SMIN_SAMPLE_VALUE",
        "SMAX_SAMPLE_VALUE",
        "TRANSFER_RANGE"
      };

static char *tiff_type[] =
      { "",
        "BYTE",
        "ASCII",
        "SHORT",
        "LONG",
        "RATIONAL",
        "SBTYE",
        "UNDEFINED",
        "SSHORT",
        "SLONG",
        "SRATIONAL",
        "FLOAT",
        "DOUBLE",
        "LONG64"
      };

void Print_Tiff_IFD(Tiff_IFD *eifd, FILE *output, int indent)
{ TIFD *ifd = (TIFD *) eifd;
  int   i, j;

  for (i = 0; i < ifd->numtags; i++)
    { Tif_Tag *tag = ifd->tags+i;
      int label = tag->label;
      int count = (int) tag->count;
      int type  = tag->type;

      uint8  *valB;
      char   *valA;
      uint16 *valS;
      uint32 *valI;
      uint64 *valO;

      fprintf(output,"%*s",indent,"");
      if (label < TIFF_NEW_SUB_FILE_TYPE)
        fprintf(output,"???");
      else if (label <= TIFF_TRANSFER_RANGE)
        fprintf(output,"%s",tiff_label[label-TIFF_NEW_SUB_FILE_TYPE]);
      else
        fprintf(output,"++ %d ++",label);

      printf(" %d %s: ",count,tiff_type[type]);

      switch (type)
      { case TIFF_BYTE:
          valB = (uint8 *) (ifd->values + tag->vector);
          for (j = 0; j < count; j++)
            { fprintf(output," %u",valB[j]);
              if (j > 5)
                { fprintf(output," ...");
                  break;
                }
            }
          break;
        case TIFF_ASCII:
          valA = (char *) (ifd->values + tag->vector);
          fprintf(output,"\'");
          for (j = 0; j < count; j++)
            { if (valA[j] < ' ')
                fprintf(output,"\\%xx",valA[j]);
              else
                fprintf(output,"%c",valA[j]);
              if (j > 50)
                { fprintf(output," ...");
                  break;
                }
            }
          fprintf(output,"\'");
          break;
        case TIFF_SHORT:
          valS = (uint16 *) (ifd->values + tag->vector);
          for (j = 0; j < count; j++)
            { fprintf(output," %u",valS[j]);
              if (j > 5)
                { fprintf(output," ...");
                  break;
                }
            }
          break;
        case TIFF_LONG:
          valI = (uint32 *) (ifd->values + tag->vector);
          for (j = 0; j < count; j++)
            { fprintf(output," %u",valI[j]);
              if (j > 5)
                { fprintf(output," ...");
                  break;
                }
            }
          break;
        case TIFF_LONG64:
          valO = (uint64 *) (ifd->values + tag->vector);
          for (j = 0; j < count; j++)
            { fprintf(output," %llu",valO[j]);
              if (j > 5)
                { fprintf(output," ...");
                  break;
                }
            }
          break;
        case TIFF_RATIONAL:
          valI = (uint32 *) (ifd->values + tag->vector);
          for (j = 0; j < count; j++)
            { fprintf(output," %d/%d",valI[2*j],valI[2*j+1]);
              if (j > 5)
                { fprintf(output," ...");
                  break;
                }
            }
          break;
        default:
          fprintf(output,"...");
          break;
      }

      if (label == TIFF_STRIP_OFFSETS || label == TIFF_TILE_OFFSETS)
        { if (ifd->source_ref == NULL)
            printf(" (image in memory)");
          else
            printf(" (image on file)");
        }

      fprintf(output,"\n");
    }
}


/****************************************************************************************
 *                                                                                      *
 *  UTILITIES FOR ENDIAN HANDLING                                                       *
 *                                                                                      *
 ****************************************************************************************/

static void flip_short(void *w)
{ uint8 *v = (uint8 *) w;
  uint8  x;
  
  x    = v[0];
  v[0] = v[1];
  v[1] = x;
}

static void flip_long(void *w)
{ uint8 *v = (uint8 *) w;
  uint8  x;

  x    = v[0];
  v[0] = v[3];
  v[3] = x;
  x    = v[1];
  v[1] = v[2];
  v[2] = x;
}

static void flip_quad(void *w)
{ uint8 *v = (uint8 *) w;
  uint8  x;

  x    = v[0];
  v[0] = v[7];
  v[7] = x;
  x    = v[1];
  v[1] = v[6];
  v[6] = x;
  x    = v[2];
  v[2] = v[5];
  v[5] = x;
  x    = v[3];
  v[3] = v[4];
  v[4] = x;
}

int Native_Endian()
{ uint32 x;
  uint8 *b;

  b = (uint8 *) (&x);
  x = 3;
  return (b[0] != 3);
}

#define INPUT_SHORT(var)				\
  if (fread(&(var),2,1,input) != 1)			\
    { if (Grab_Message_8qm5())				\
        sprintf(Tiff_Estring,"File ends prematurely");	\
      goto error;					\
    }							\
  if (flip)						\
    flip_short(&(var));

#define INPUT_LONG(var)					\
  if (fread(&(var),4,1,input) != 1)			\
    { if (Grab_Message_8qm5())				\
        sprintf(Tiff_Estring,"File ends prematurely");	\
      goto error;					\
    }							\
  if (flip)						\
    flip_long(&(var));

#define INPUT_QUAD(var)					\
  if (fread(&(var),8,1,input) != 1)			\
    { if (Grab_Message_8qm5())				\
        sprintf(Tiff_Estring,"File ends prematurely");	\
      goto error;					\
    }							\
  if (flip)						\
    flip_quad(&(var));

#define INPUT_OFFSET(var)				\
  { if (flag)						\
      { INPUT_QUAD(var) }				\
    else						\
      { uint32 _t;					\
        INPUT_LONG(_t)					\
        var = _t;					\
      }							\
}

#define OUTPUT_SHORT(var)		\
  { uint16 _t = (uint16) (var);		\
    if (flip)				\
      flip_short(&_t);                  \
    fwrite(&_t,2,1,output);		\
  }

#define OUTPUT_LONG(var)		\
  { uint32 _t = (uint32) (var);		\
    if (flip)				\
      flip_long(&_t);                   \
    fwrite(&_t,4,1,output);		\
  }

#define OUTPUT_QUAD(var)		\
  { uint64 _t = (var);			\
    if (flip)				\
      flip_quad(&_t);                   \
    fwrite(&_t,8,1,output);		\
  }

#define OUTPUT_OFFSET(var)		\
  { if (flag)				\
      OUTPUT_QUAD(var)			\
    else				\
      OUTPUT_LONG(var)			\
  }

static int optseek(FILE *file, off_t offset, int pos)
{ if (pos == SEEK_SET)
    { off_t p = (off_t) ftello(file);
      if (p < 0)
        return ((int) p);
      else if (offset != p)
        return (fseeko(file,offset,pos));
      else
        return (0);
    }
  else //  pos == SEEK_END or pos == SEEK_CUR
    return (fseeko(file,offset,pos));
}

/****************************************************************************************
 *                                                                                      *
 *  TIFF_READER ROUTINES                                                                *
 *                                                                                      *
 ****************************************************************************************/

Tiff_Reader *Open_Tiff_Reader(char *name, int *big_endian, int *flag64, int lsm)
{ int           mach_endian;
  struct stat   fdesc;

  Treader *tif;
  int      flag;
  uint64   offset;
  int      flip;
  uint16   order;
  FILE    *input;

  mach_endian = Native_Endian();

  input = fopen(name,"rb");
  if (input == NULL)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Cannot open file %s for reading",name);
      goto error;
    }

  if (fread(&order,2,1,input) != 1)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"File ends prematurely");
      goto error;
    }
  if (order == 0x4949)
    { flip = mach_endian;
      if (big_endian != NULL)
        *big_endian = 0;
    }
  else if (order == 0x4D4D)
    { flip = 1-mach_endian;
      if (big_endian != NULL)
        *big_endian = 1;
    }
  else
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"File %s does not contain valid endian value",name);
      goto error;
    }

  INPUT_SHORT(order)

  flag = 0;
  if (order == 0x0040)
    flag = 1;
  else if (order != 0x002A)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"File %s does not contain a valid magic key",name);
      goto error;
    }
  if (flag64 != NULL)
    *flag64 = flag;

  if (flag && lsm)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"File %s cannot be an lsm file as it has 64-bit offsets",name);
      goto error;
    }

  INPUT_OFFSET(offset)

  tif = new_treader(NULL);
  if (tif == NULL) goto error;

  fstat(fileno(input),&fdesc);

  tif->flip       = flip;
  tif->flag64     = flag;
  tif->first_ifd  = offset;
  tif->ifd_offset = offset;
  tif->ifd_no     = 1;
  tif->lsm        = lsm;
  tif->file_size  = (uint64) fdesc.st_size;
  tif->input      = input;

  return ((Tiff_Reader *) tif);

error:
  if (my_message())
    sprintf(Tiff_Esource,"Open_Tiff_Reader");
  if (input != NULL)
    fclose(input);
  return (NULL);
}

void Rewind_Tiff_Reader(Tiff_Reader *rtif)
{ Treader *tif = (Treader *) rtif;
  tif->ifd_no     = 1;
  tif->ifd_offset = tif->first_ifd;
}

int Advance_Tiff_Reader(Tiff_Reader *rtif)
{ Treader *tif = (Treader *) rtif;
  uint16   ntags;
  int      flip, flag;
  int64    offset;
  FILE    *input;

  if (tif->ifd_offset == 0)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Trying to advance at end-of-ifd-list");
      goto error;
    }

  input  = tif->input;
  flip   = tif->flip;
  flag   = tif->flag64;
  offset = (int64) tif->ifd_offset;

  if (optseek(input,(off_t) offset,SEEK_SET) < 0)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Seek for next IFD failed");
      goto error;
    }

  INPUT_SHORT(ntags)

  if (tif->flag64)
    offset += 2 + ntags*16;
  else
    offset += 2 + ntags*12;

  if (optseek(input,(off_t) offset,SEEK_SET) < 0)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Seek for next IFD failed");
      goto error;
    }

  INPUT_OFFSET(offset);

  tif->ifd_no    += 1;
  tif->ifd_offset = (uint64) offset;

  return (0);

error:
  if (my_message())
    sprintf(Tiff_Esource,"Advance_Tiff_Reader");
  return (1);
}

int End_Of_Tiff(Tiff_Reader *tif)
{ return (((Treader *) tif)->ifd_offset == 0); }

/* For .lsm's produced by Zeiss: the ROWS_PER_STRIP tag is missing and equals IMAGE_LENGTH.
   For .lsm's subsequently written by this package, the ROWS_PER_STRIP tag is present, implying
   the file is a proper tif (including byte counts) save for the bloody BITS_PER_PIXEL value
*/

static int get_lsm_size(uint32 offset, int y, int flip, FILE *input)
{ uint32 size;

  if (optseek(input,((off_t) offset)+y,SEEK_SET) < 0)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Seek to lsm sub-block failed");
      return (-1);
    }
  if (fread(&size,4,1,input) != 1)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"File ends prematurely");
      return (-1);
    }
  if (flip)
    flip_long(&size);
  return ((int) size);
}

Tiff_IFD *Read_Tiff_IFD(Tiff_Reader *rtif)
{ Treader *tif = (Treader *) rtif;
  uint16   ntags;
  int      flip, flag, obytes;
  Tif_Tag *off_tag, *cnt_tag;
  FILE    *input;
  TIFD    *ifd;

  Tif_Tag *bit_tag, *spp_tag, *map_tag, *lsm_tag;   //  lsm correction variables
  int      lsm_lzw, has_row;

  ifd = NULL;

  if (tif->ifd_offset == 0)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Trying to read when at end-of-ifd-list");
      goto error;
    }

  input = tif->input;
  flip  = tif->flip;
  flag  = tif->flag64;
  if (flag)
    obytes = 8;
  else
    obytes = 4;

  // Seek to the next IFD, see how many tags it has,
  //   and allocate the ifd record and its tag array

  if (optseek(input,(off_t) tif->ifd_offset,SEEK_SET) < 0)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Seek for next IFD failed");
      goto error;
    }

  INPUT_SHORT(ntags)

  ifd = new_tifd((ntags+10)*sizeof(Tif_Tag),0,NULL);
  if (ifd == NULL)
    goto error;

  ifd->data_flip  = flip; 
  ifd->numtags    = ntags;
  ifd->maxtags    = ntags+10;
  ifd->source_ref = Inc_Tiff_Reader(rtif);

  // Load the tags, endian treating all but the value field

  { int i, veof;

    bit_tag = NULL;   //  Initially all lsm variables are "off"
    spp_tag = NULL;
    map_tag = NULL;
    lsm_tag = NULL;
    lsm_lzw = 0;
    has_row = 0;

    veof  = 0;
    for (i = 0; i < ntags; i++)

      { uint16 label, type;
        uint32 count;

        Tif_Tag *tag = ifd->tags+i;

        INPUT_SHORT(label);
        INPUT_SHORT(type);
        INPUT_LONG(count);
        if (fread(POINTER(tag),(size_t) obytes,1,input) != 1)
          { if (Grab_Message_8qm5())
              sprintf(Tiff_Estring,"File ends prematurely");
            goto error;
          }

        if (type < TIFF_BYTE || type > TIFF_LONG64)
          { if (Grab_Message_8qm5())
              sprintf(Tiff_Estring,"Illegal type, %d, in IFD tag",type);
            goto error;
          }

        //  If lsm then (a) may have to fix BITS_PER_SAMPLE tag if more than 1 channel,
        //    (b) may have to fix color map if present, and (c) will have to bundle lsm's
        //    special tag and its auxiliary blocks

        if (tif->lsm)
          { if (label == TIFF_SAMPLES_PER_PIXEL)
              spp_tag = tag;
            else if (label == TIFF_COLOR_MAP)
              map_tag = tag;
            else if (label == TIFF_CZ_LSM_INFO && tif->ifd_no == 1)
              lsm_tag = tag;
            else if (label == TIFF_BITS_PER_SAMPLE && count > 1)
              bit_tag = tag;
            else if (label == TIFF_ROWS_PER_STRIP)
              has_row = 1;
          }

        tag->label = label;
        tag->type  = type;
        tag->count = count;

        veof  += WORD_MULTIPLE(((int) count) * type_sizes[type]);
      }

    ifd->veof  = veof;
    ifd->vmax  = veof + 1024;

    if (allocate_tifd_values(ifd,ifd->vmax,NULL)) goto error;
  }

  // Get the offset of the next IFD

  { uint64 offset;

    INPUT_OFFSET(offset);
    tif->ifd_no    += 1;
    tif->ifd_offset = offset;
  }

  // Get the value arrays of tags whose values don't fit in the value field, endian adjust
  //   the values in all cases, and incidentally note the count and offset tags and whether
  //   this is a compressed lsm

  { int    i;
    uint64 veof;

    veof = 0;
    off_tag = cnt_tag = NULL;
    for (i = 0; i < ntags; i++)

      { Tif_Tag *tag = ifd->tags+i;
        int      tsize  = type_sizes[tag->type];
        int      esize  = tsize * ((int) tag->count);

        if (tag->label == TIFF_STRIP_OFFSETS || tag->label == TIFF_TILE_OFFSETS)
          { off_tag = tag;
            if ((flag && tag->type != TIFF_LONG64) || (!flag && tag->type != TIFF_LONG &&
                            (tag->type != TIFF_SHORT || tag->label != TIFF_STRIP_OFFSETS)))
              { if (Grab_Message_8qm5())
                  sprintf(Tiff_Estring,"Type, %d, of image offsets is not correct",tag->type);
                goto error;
              }
          }
        else if (tag->label == TIFF_STRIP_BYTE_COUNTS || tag->label == TIFF_TILE_BYTE_COUNTS)
          { cnt_tag = tag;
            if (tag->type != TIFF_LONG && tag->type != TIFF_SHORT && tag->type != TIFF_LONG64)
              { if (Grab_Message_8qm5())
                  sprintf(Tiff_Estring,"Type, %d, of image byte counts is not correct",tag->type);
                goto error;
              }
          }
      
        { uint64 *valO;        //  value is in a block, seek to it, read into the end
          uint32 *valI;        //    of the values block, and endian treat its values
          uint16 *valS;
          void   *optr;
          uint32     j;
          uint64     o;

          optr = ifd->values+veof;

          if (esize <= obytes && tag != bit_tag)
            memcpy(optr,POINTER(tag),(size_t) esize);
          else
            { if (flag)
                { valO = POINTER(tag);
                  if (flip)
                    flip_quad(valO);
                  o = *valO;
                }
              else
                { valI = (uint32 *) POINTER(tag);
                  if (flip)
                    flip_long(valI);
                  o = *valI;
                }
              if (optseek(input,(off_t) o,SEEK_SET) < 0)
                { if (Grab_Message_8qm5())
                    sprintf(Tiff_Estring,"Seek to tag value vector failed");
                  goto error;
                }

              if (fread(optr,(size_t) esize,1,input) != 1)
                { if (Grab_Message_8qm5())
                    sprintf(Tiff_Estring,"File ends prematurely");
                  goto error;
                }
            }
          tag->vector = veof;
          veof += (uint64) WORD_MULTIPLE(esize);

          if (flip)
            switch (tsize)
            { case 2:
                valS = (uint16 *) optr;
                for (j = 0; j < tag->count; j++)
                  flip_short(valS+j);
                break;
              case 4:
                valI = (uint32 *) optr;
                for (j = 0; j < tag->count; j++)
                  flip_long(valI+j);
                break;
              case 8:
                if (tag->type == TIFF_DOUBLE || tag->type == TIFF_LONG64)
                  { valO = (uint64 *) optr;
                    for (j = 0; j < tag->count; j++)
                      flip_quad(valO+j);
                  }
                else  // tag->type in { TIFF_RATIONAL, TIFF_SRATIONAL }
                  { valI = (uint32 *) optr;
                    for (j = 0; j < 2*tag->count; j++)
                      flip_long(valI+j);
                  }
                break;
            }
        }

        //  Set flag if lzw compressed lsm file that was not written by us (sigh)

        if (tif->lsm && tag->label == TIFF_COMPRESSION
                     && *((uint16 *) (ifd->values+tag->vector)) == TIFF_VALUE_LZW)
          lsm_lzw = 1 - has_row;
      }

    if (off_tag == NULL)
      { if (Grab_Message_8qm5())
          sprintf(Tiff_Estring,"IFD does not contain an offset tag");
        goto error;
      }
    if (cnt_tag == NULL)
      { if (Grab_Message_8qm5())
          sprintf(Tiff_Estring,"IFD does not contain an byte_count tag");
        goto error;
      }
    if ((off_tag->label == TIFF_STRIP_OFFSETS) != (cnt_tag->label == TIFF_STRIP_BYTE_COUNTS))
      { if (Grab_Message_8qm5())
          sprintf(Tiff_Estring,"IFD does not contain compatible offset and byte_count tags");
        goto error;
      }
    if (off_tag->count != cnt_tag->count)
      { if (Grab_Message_8qm5())
          sprintf(Tiff_Estring,"Offset and byte_count tags do not have the same length");
        goto error;
      }
  }

  //  If this is an lsm file and the info tag is present, gather the blocks into one super block
  //    so that it becomes a proper tif

  /* Notes:
     * space for TOPO_ISOLINE_OVERLAY is twice what it needs to be (200 vs. 400), all other
         overlays are 200 and space for them is the same
     * the size of the CHANNEL_COLORS is coded as 70 but is in fact 72 bytes are needed and 138
         bytes are available to it.  Added NO_CHANNELS as I figure they just forgot to count the
         '\0's at the end of each color name.
     * KS_DATA and UNMIX_PARAMETERS are undocumented
  */

  if (lsm_tag != NULL)
    { int    lflip, slot;
      uint32 vsize;
      uint32 lsm_subsizes[LSM_ASCII_SIGNATURE+1];

      uint32 *valI = (uint32 *) (ifd->values + lsm_tag->vector);

      { uint32 key;   //  Determine if multi-byte values need to be flipped

        key = valI[0];
        if (key == 0x0300494C || key == 0x0400494C)
          lflip = 0;
        else
          { lflip = 1;
            flip_long(&key);
            if (key != 0x0300494C && key != 0x0400494C)
              { if (Grab_Message_8qm5())
                  sprintf(Tiff_Estring,"LSM tag does not start with valid magic key");
                goto error;
              }
          }
      }

      if (lflip)       //  Flip all the offset vars in the main block if needed
        { int i, idx;

          for (i = 0; (idx = lsm_offset_list[i]) != 0; i++)
            flip_long(valI+idx);
          flip_long(valI+LSM_NO_CHANNELS);
          flip_short(((uint16 *) valI) + LSM_SCAN_TYPE);
        }

      //  Determine the sum of the sizes of all blocks

      vsize = lsm_tag->count;

      { int    i, idx;
        uint32 offset;

#define LSM_FETCH(channel,disp,mul,add)			\
  if ((offset = valI[channel]) != 0)			\
    { int s = get_lsm_size(offset,disp,lflip,input);	\
      if (s < 0) goto error;				\
      lsm_subsizes[channel] = mul*((uint32) s)+add;	\
      vsize += lsm_subsizes[channel];			\
    }							\
  else							\
    lsm_subsizes[channel] = 0;

        for (i = 0; (idx = lsm_1st_size[i]) != 0; i++)
          LSM_FETCH(idx,0,1,0);

        for (i = 0; (idx = lsm_2nd_size[i]) != 0; i++)
          LSM_FETCH(idx,4,1,0);
  
        for (i = 0; (idx = lsm_zero_size[i]) != 0; i++)
          { valI[idx] = 0;
            lsm_subsizes[idx] = 0;
          }
      
        LSM_FETCH(LSM_CHANNEL_COLORS,0,1,valI[LSM_NO_CHANNELS])
        LSM_FETCH(LSM_CHANNEL_WAVELENGTH,0,16,4)

        if (((uint16 *) valI)[LSM_SCAN_TYPE] != 5)
          { valI[LSM_MEAN_OF_ROIS_OVERLAY] = 0;
            lsm_subsizes[LSM_MEAN_OF_ROIS_OVERLAY] = 0;
          }
        else
          LSM_FETCH(LSM_MEAN_OF_ROIS_OVERLAY,4,1,0)
      }

      vsize += (lsm_subsizes[LSM_CHANNEL_DATA_TYPES] = valI[LSM_NO_CHANNELS] * 4);
      vsize += (lsm_subsizes[LSM_CHANNEL_FACTORS] = valI[LSM_NO_CHANNELS] * 24);

      { uint32 vecI[3], depth;   //  Get size of SCAN_INFORMATION by traversing it (the only way!)
        uint32 size;
        
        if (optseek(input,(off_t) valI[LSM_SCAN_INFORMATION],SEEK_SET) < 0)
          { if (Grab_Message_8qm5())
              sprintf(Tiff_Estring,"Seek to lsm sub-block for scan information failed");
            goto error;
          }
        depth = 0;
        size  = 0;
        do
          { if (fread(vecI,12,1,input) != 1)
              { if (Grab_Message_8qm5())
                  sprintf(Tiff_Estring,"Read within lsm scan information sub-block failed");
                goto error;
              }
            if (lflip)
              { flip_long(vecI);
                flip_long(vecI+2);
              }
            if ((vecI[0] & 0xff) == 0)
              depth += 1;
            else if ((vecI[0] & 0xff) == 0xff)
              depth -= 1;
            size += 12 + vecI[2];
            if (optseek(input,(off_t) vecI[2],SEEK_CUR) < 0)
              { if (Grab_Message_8qm5())
                  sprintf(Tiff_Estring,"Seek within lsm scan information sub-block failed");
                goto error;
              }
          }
        while (depth != 0);

        lsm_subsizes[LSM_SCAN_INFORMATION] = size;
        vsize += size;
      }

      // Allocate a space big enough for all the blocks and move them into it

      slot = ifd->veof;

      ifd->veof  += WORD_MULTIPLE(vsize);
      ifd->vmax   = ifd->veof + 1024;
  
      if (allocate_tifd_values(ifd,ifd->vmax,NULL)) goto error;

      valI = (uint32 *) (ifd->values + slot);
      memcpy(valI,ifd->values + lsm_tag->vector,lsm_tag->count);
      lsm_tag->vector = (uint64) slot;
      slot += (int) lsm_tag->count;
      lsm_tag->count = vsize;
  
      { int    i, idx;
        uint32 offset;

        for (i = 0; (idx = lsm_offset_list[i]) != 0; i++)
          if ((offset = valI[idx]) != 0)
            { optseek(input,(off_t) offset,SEEK_SET);
              fread(ifd->values+slot,lsm_subsizes[idx],1,input);
              valI[idx] = (uint32) (((uint64) slot)-lsm_tag->vector);
              slot += lsm_subsizes[idx];
            }
      }

      if (lflip)
        { int i, idx;

          for (i = 0; (idx = lsm_offset_list[i]) != 0; i++)
            flip_long(valI+idx);
          flip_long(valI+LSM_NO_CHANNELS);
          flip_short(((uint16 *) valI) + LSM_SCAN_TYPE);
        }
    }

  // LSM corrections: determine correct counts for compressed data, shift COLOR_MAP if necessary,
  //                  add ROWS_PER_STRIP tag, and fix/add PHOTOMETRIC_INTERPRETATION

  if (tif->lsm && ! has_row)

    { if (lsm_lzw)
        { uint64 *cntO = NULL;
          uint64 *offO = NULL;
          uint32 *cntI = NULL;
          uint32 *offI = NULL;
          uint16 *cntS = NULL;
          uint16 *offS = NULL;
          uint32  i;
          int     ctype, otype;

          ctype = cnt_tag->type;
          if (ctype == TIFF_SHORT)
            cntS = (uint16 *) (ifd->values + cnt_tag->vector);
          else if (ctype == TIFF_LONG)
            cntI = (uint32 *) (ifd->values + cnt_tag->vector);
          else //  ctype == TIFF_LONG64
            cntO = (uint64 *) (ifd->values + cnt_tag->vector);

          otype = off_tag->type;
          if (otype == TIFF_SHORT)
            offS = (uint16 *) (ifd->values + off_tag->vector);
          else if (otype == TIFF_LONG)
            offI = (uint32 *) (ifd->values + off_tag->vector);
          else // otype == TIFF_LONG64
            offO = (uint64 *) (ifd->values + off_tag->vector);

          for (i = 0; i < off_tag->count; i++)
            { uint64 offset, csize;

              if (otype == TIFF_SHORT)
                offset  = offS[i];
              else if (otype == TIFF_LONG)
                offset  = offI[i];
              else
                offset  = offO[i];
 
              if (optseek(input,(off_t) offset,SEEK_SET) < 0)
                goto error;
              csize = LZW_Counter_8qm5(input);

              if (ctype == TIFF_SHORT)
                cntS[i] = (uint16) csize;
              else if (ctype == TIFF_LONG)
                cntI[i] = (uint32) csize;
              else
                cntO[i] = csize;
            }
        }
 
      if (map_tag != NULL)         //  Rescale the color map if the upper byte was not used.
        { int     highbit, i;
          uint16 *valS = (uint16 *) (ifd->values+map_tag->vector);

          highbit = 0;
          for (i = 0; i < (int) map_tag->count; i++)
            if (valS[i] > 256)
              highbit = 1;
          if ( ! highbit)
            { for (i = 0; i < (int) map_tag->count; i++)
                valS[i] <<= 8;
            }
        }

      { Tiff_Type type;
        int       count;
        uint32   *ptr;

        ptr = (uint32 *) Get_Tiff_Tag(ifd,TIFF_IMAGE_LENGTH,&type,&count);
        if (ptr == NULL)
          { if (Grab_Message_8qm5())
              sprintf(Tiff_Estring,"Lsm file does not have image_length tag");
            goto error;
          }
        if (Set_Tiff_Tag(ifd,TIFF_ROWS_PER_STRIP,TIFF_LONG,1,ptr)) goto error;
      }

      { uint16 count;

        if (spp_tag != NULL)
          count = *((uint16 *) (ifd->values + spp_tag->vector));
        else
          count = 1;
        if (count == 3)
          count = TIFF_VALUE_RGB;
        else if (count == 1 && map_tag != NULL)
          count = TIFF_VALUE_RGB_PALETTE;
        else
          count = TIFF_VALUE_BLACK_IS_ZERO;
        if (Set_Tiff_Tag(ifd,TIFF_PHOTOMETRIC_INTERPRETATION,TIFF_SHORT,1,&count)) goto error;
      }
    }

  return ((Tiff_IFD *) ifd);

error:
  if (ifd != NULL)
    Free_Tiff_IFD(ifd);
  if (my_message())
    sprintf(Tiff_Esource,"Read_Tiff_IFD"); 
  return (NULL);
}


/****************************************************************************************
 *                                                                                      *
 *  TIFF_IFD ROUTINES                                                                   *
 *                                                                                      *
 ****************************************************************************************/

Tiff_IFD *Create_Tiff_IFD(int num_tags, void *image)
{ TIFD *ifd;
  
  ifd = new_tifd((num_tags+10)*((int) sizeof(Tif_Tag)),1024,NULL);
  if (ifd == NULL) goto error;

  ifd->data_flip = 0;
  ifd->numtags   = 0;
  ifd->maxtags   = num_tags+10;

  ifd->vmax   = 1024;
  ifd->veof   = 0;

  ifd->data   = image;

  return ((Tiff_IFD *) ifd);

error:
  if (my_message())
    sprintf(Tiff_Esource,"Create_Tiff_IFD");
  return (NULL);
}

void *Get_Tiff_Tag(Tiff_IFD *eifd, int label, Tiff_Type *type, int *count)
{ TIFD *ifd = (TIFD *) eifd;
  int   i;

  for (i = 0; i < ifd->numtags; i++)
    if (ifd->tags[i].label == label)
      { Tif_Tag *tag = ifd->tags+i;

        if (type != NULL)
          *type  = tag->type;
        if (count != NULL)
          *count = (int) tag->count; 
        return (ifd->values + tag->vector);
      }
  if (Grab_Message_8qm5())
    { sprintf(Tiff_Esource,"Get_Tiff_Tag");
      if (TIFF_NEW_SUB_FILE_TYPE <= label && label <= TIFF_TRANSFER_RANGE)
        sprintf(Tiff_Estring,"No %s tag in IFD",tiff_label[label-TIFF_NEW_SUB_FILE_TYPE]);
      else
        sprintf(Tiff_Estring,"No tag, %d, in IFD",label);
    }
  return (NULL);
}

void Delete_Tiff_Tag(Tiff_IFD *eifd, int label)
{ TIFD *ifd = (TIFD *) eifd;
  int   i;

  for (i = 0; i < ifd->numtags; i++)
    if (ifd->tags[i].label == label)
      { for (i++; i < ifd->numtags; i++)
          ifd->tags[i-1] = ifd->tags[i];
        ifd->numtags -= 1;
        break;
      }
}

void *Allocate_Tiff_Tag(Tiff_IFD *eifd, int label, Tiff_Type type, int count)
{ TIFD    *ifd = (TIFD *) eifd;

  Tif_Tag *tag;
  int      nsize, osize;
  int      i;

  if (count <= 0)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Non-positive count");
      goto error;
    }
  if (type < TIFF_BYTE || type > TIFF_LONG64)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Invalid Type, %d",type);
      goto error;
    }

  nsize = type_sizes[type]*count;
  for (i = 0; i < ifd->numtags; i++)
    if (ifd->tags[i].label == label)
      { tag   = ifd->tags+i;
        osize = type_sizes[tag->type]*((int) tag->count);
        break;
      }
  if (i >= ifd->numtags)
    { if (i >= ifd->maxtags)
        { if (allocate_tifd_tags(ifd,(ifd->maxtags+10)*((int) sizeof(Tif_Tag)),NULL)) goto error;
          ifd->maxtags += 10;
        }
      tag = ifd->tags + i; 
      osize = 0;
    }

  if (osize < nsize)
    { osize = WORD_MULTIPLE(nsize);
      if (ifd->veof + osize >= ifd->vmax) 
        { int vmax = ifd->veof + osize + 1024;
          if (allocate_tifd_values(ifd,vmax,NULL)) goto error;
          ifd->vmax = vmax;
        }
      tag->vector = (uint64) ifd->veof;
      ifd->veof  += osize;
    }

  tag->label = (uint16) label;
  tag->type  = type;
  tag->count = (uint32) count; 

  if (i >= ifd->numtags)
    ifd->numtags += 1;

  return (ifd->values + tag->vector); 

error:
  if (my_message())
    sprintf(Tiff_Esource,"Allocate_Tiff_Tag");
  return (NULL);
}

int Set_Tiff_Tag(Tiff_IFD *eifd, int label, Tiff_Type type, int count, void *data)
{ void *ptr;

  ptr = Allocate_Tiff_Tag(eifd,label,type,count);
  if (ptr != NULL)
    { memcpy(ptr,data,(size_t) (type_sizes[type]*count));
      return (0);
    }
  else
    { if (my_message())
        sprintf(Tiff_Esource,"Set_Tiff_Tag");
      return (1);
    }
}

int Count_Tiff_Tags(Tiff_IFD *eifd)
{ TIFD *ifd = (TIFD *) eifd;
  return (ifd->numtags);
}

int Get_Tiff_Label(Tiff_IFD *eifd, int idx)
{ TIFD *ifd = (TIFD *) eifd;
  if (idx < 0 || idx >= ifd->numtags)
    { if (Grab_Message_8qm5())
        { sprintf(Tiff_Estring,"Index is out of range");
          sprintf(Tiff_Esource,"Get_Tiff_Label");
        }
      return (0);
    }
  return (ifd->tags[idx].label);
}

FILE *Get_Tiff_IFD_Stream(Tiff_IFD *ifd)
{ Tiff_Reader *reader = ((TIFD *) ifd)->source_ref;
  if (reader == NULL)
    return (NULL);
  return (((Treader *) reader)->input);
}

FILE *Get_Tiff_IFD_Image(Tiff_IFD *ifd)
{ return (((TIFD *) ifd)->data); }

int Get_Tiff_IFD_Data_Flip(Tiff_IFD *ifd)
{ return (((TIFD *) ifd)->data_flip); }


/****************************************************************************************
 *                                                                                      *
 *  TIFF_WRITER ROUTINES                                                                *
 *                                                                                      *
 ****************************************************************************************/

Tiff_Writer *Open_Tiff_Writer(char *name, int flag64, int lsm)
{ Twriter *tif;
  FILE    *output;

  output = NULL;

  if (flag64 && lsm)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"An lsm file cannot have 64-bit offsets");
      goto error;
    }

  output = fopen(name,"wb");
  if (output == NULL)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Cannot open file %s for writing",name);
      goto error;
    }

  tif = new_twriter(0,NULL);
  if (tif == NULL)
    goto error;

  tif->ifd_no = 1;
  tif->output = output;
  tif->lsm    = lsm;
  tif->flag64 = flag64;

  return ((Tiff_Writer *) tif);

error:
  if (output != NULL)
    fclose(output);
  if (my_message())
    sprintf(Tiff_Esource,"Open_Tiff_Writer");
  return (NULL);
}

static void write_tiff_header(Twriter *tif, int flip)
{ int      mach_endian;
  uint64   offset;
  int      flag;
  uint16   order;
  FILE    *output;

  mach_endian = Native_Endian();

  flag   = tif->flag64;
  output = tif->output;
 
  if (flip == mach_endian)
    order = 0x4949;
  else
    order = 0x4D4D;
  fwrite(&order,2,1,output);

  if (flag)
    { OUTPUT_SHORT(0x0040)
      offset = 12;
    }
  else
    { OUTPUT_SHORT(0x002A)
      offset = 8;
    }

  OUTPUT_OFFSET(offset)

  tif->flip       = flip;
  tif->eof_offset = offset;
  tif->ifd_linko  = 4;
  tif->ano_linko  = 0;
  tif->ano_count  = 0;
}

static int TAG_SORT(const void *x, const void *y)
{ Tif_Tag *a = (Tif_Tag *) x;
  Tif_Tag *b = (Tif_Tag *) y;
  return (a->label - b->label);
}

int Write_Tiff_IFD(Tiff_Writer *etif, Tiff_IFD *eifd)
{ TIFD    *ifd = (TIFD *) eifd;
  Twriter *tif = (Twriter *) etif;
  
  Tif_Tag *cnt_tag = NULL;
  Tif_Tag *off_tag = NULL;
  Tif_Tag *lsm_tag = NULL;
  Tif_Tag *bps_tag = NULL;

  int      ctype = 0;
  int      otype = 0;
  uint64  *offO = NULL;
  uint32  *offI = NULL;
  uint16  *offS = NULL;
  uint64  *cntO = NULL;
  uint32  *cntI = NULL;
  uint16  *cntS = NULL;

  int      flip, flag, obytes;
  FILE    *output;
  int      block_size, vector_size;
  uint64   block_offset, data_offset;

  if (tif->ifd_no == 1)
    write_tiff_header(tif,ifd->data_flip);
  else if (tif->flip != ifd->data_flip)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Writing IFD's with different endians to same file");
      goto error;
    }
 
  output = tif->output;
  flip   = tif->flip;
  flag   = tif->flag64;
  if (flag)
    obytes = 8;
  else
    obytes = 4;

  { char     *desc;         //  Hack, ImageJ prior to 1.4 versions makes assumptions about the
    Tiff_Type type;         //    arrangement of blocks if it finds a description that it produced!
    int       count = 0;    //    Must get rid of it, or ImageJ won't read the tiff produced.

    desc = Get_Tiff_Tag(eifd,TIFF_IMAGE_DESCRIPTION,&type,&count);
    if (desc != NULL && count >= 10 && strncmp(desc,"ImageJ=1.",9) == 0 && desc[9] < '4')
      Delete_Tiff_Tag(eifd,TIFF_IMAGE_DESCRIPTION);
  }

  qsort(ifd->tags,(size_t) ifd->numtags,sizeof(Tif_Tag),TAG_SORT);

  { int i;

    vector_size = 0;
    for (i = 0; i < ifd->numtags; i++)
      { Tif_Tag *tag  = ifd->tags+i;
        int      label = tag->label;
        int      count = (int) tag->count;
        int      esize = count * type_sizes[tag->type];

        switch (label)
        { case TIFF_STRIP_BYTE_COUNTS:
          case TIFF_TILE_BYTE_COUNTS:
            cnt_tag = tag;
            ctype   = tag->type;
            if (ctype == TIFF_SHORT)
              cntS = (uint16 *) (ifd->values + tag->vector);
            else if (ctype == TIFF_LONG)
              cntI = (uint32 *) (ifd->values + tag->vector);
            else
              cntO = (uint64 *) (ifd->values + tag->vector);
            break;
          case TIFF_STRIP_OFFSETS:
          case TIFF_TILE_OFFSETS:
            off_tag = tag;
            otype   = tag->type;
            if (flag)
              tag->type = TIFF_LONG64;
            else
              tag->type = TIFF_LONG;
            esize = count * type_sizes[tag->type];
            if (otype == TIFF_SHORT)
              offS = (uint16 *) (ifd->values + tag->vector);
            else if (otype == TIFF_LONG)
              offI = (uint32 *) (ifd->values + tag->vector);
            else //  otype == TIFF_LONG64
              offO = (uint64 *) (ifd->values + tag->vector);
            break;
          case TIFF_JF_ANO_BLOCK:
            if (tif->ifd_no == 1 && count > obytes)
              { vector_size -= count;
                if (allocate_twriter_annotation(tif,count,NULL)) goto error;
              }
            break;
          case TIFF_BITS_PER_SAMPLE:
            if (tag->type != TIFF_SHORT)
              { if (Grab_Message_8qm5())
                  sprintf(Tiff_Estring,"Bits_per_sample tag is not of type SHORT");
                goto error;
              }
            if (tif->lsm && count == 2)
              { bps_tag      = tag;
                vector_size += 4;
              }
            break;
          case TIFF_CZ_LSM_INFO:
            if (tif->ifd_no == 1 && tif->lsm)
              lsm_tag = tag;
            break;
        }

        if (esize > obytes)
          vector_size += esize;
      }
  }

  if (tif->lsm && tif->ifd_no == 1 && lsm_tag == NULL)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"First IFD does not contain an LSM info tag");
      goto error;
    }
  if (off_tag == NULL)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"IFD does not contain an offset tag");
      goto error;
    }
  if (cnt_tag == NULL)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"IFD does not contain a byte_count tag");
      goto error;
    }
  if ((off_tag->label == TIFF_STRIP_OFFSETS) != (cnt_tag->label == TIFF_STRIP_BYTE_COUNTS))
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"IFD does not contain compatible offset and byte_count tags");
      goto error;
    }
  if (off_tag->count != cnt_tag->count)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Offset and byte_count tags do not have the same length");
      goto error;
    }

  // Write image blocks

  block_size   = ifd->numtags*(8+obytes) + (2+obytes);
  block_offset = tif->eof_offset;
  data_offset  = tif->eof_offset + (uint64) (block_size + vector_size);

  fwrite(ifd->tags,(size_t) block_size,1,output);       //   Skip over IFD & vectors
  fwrite(ifd->values,(size_t) vector_size,1,output);

  if (ifd->source_ref != NULL)

    { uint32 i;                 //  Data is in another file, read and write a copy
      uint64 c, off, csize;
      FILE  *input;
 
      input    = ((Treader *) ifd->source_ref)->input;
      for (i = 0; i < off_tag->count; i++)
        { if (ctype == TIFF_SHORT)
            csize = cntS[i];
          else if (ctype == TIFF_LONG)
            csize = cntI[i];
          else
            csize = cntO[i];
          if (otype == TIFF_LONG)
            off = offI[i];
          else if (otype == TIFF_LONG64)
            off = offO[i];
          else //  otype == TIFF_SHORT
            off = offS[i];
          if (optseek(input,(off_t) off,SEEK_SET) < 0)
            { if (Grab_Message_8qm5())
                sprintf(Tiff_Estring,"Seek to image block failed");
              goto error;
            }
          for (c = 0; c < csize; c++)
            fputc(fgetc(input),output);
        }
    }

  else
    { if (Write_IFD_Image_8qm5(eifd,output))
        goto error;
    }

  tif->eof_offset = (uint64) ftello(output);

  // Write IFD block

  fseeko(output,(off_t) block_offset,SEEK_SET);

  OUTPUT_SHORT(ifd->numtags)

  { int    i, j;
    uint64 doff, voff;
    uint8  zeros[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

    voff = block_offset + ((uint64) block_size);
    doff = data_offset;

    for (i = 0; i < ifd->numtags; i++)
      { Tif_Tag *tag  = ifd->tags+i;
        int      tsize = type_sizes[tag->type];
        int      count = (int) tag->count;
        int      esize = tsize * count;
        int      label = tag->label;

        OUTPUT_SHORT(tag->label)
        OUTPUT_SHORT(tag->type)
        if (tag != lsm_tag)       //  If want an lsm then will write multiple blocks
          OUTPUT_LONG(count)
        else
          { uint32 *valI  = (uint32 *) (ifd->values + tag->vector);
            int     lflip = (valI[0] != 0x0300494C && valI[0] != 0x0400494C);

            if (lflip)
              flip_long(valI+1);
            OUTPUT_LONG(valI[1])
            esize = tsize * ((int) valI[1]);
            if (lflip)
              flip_long(valI+1);
          }

        if (label == TIFF_JF_ANO_BLOCK && tif->ifd_no == 1)
          if (count <= obytes)
            fwrite(ifd->values + tag->vector,(size_t) obytes,1,output);
          else
            { tif->ano_linko = (uint64) ftello(output);
              tif->ano_count = (uint32) count;
              memcpy(tif->annotation,ifd->values + tag->vector,(size_t) count);
              OUTPUT_OFFSET(doff)   // place holder, will be filled in at end
            }
        else if (esize <= obytes && bps_tag != tag)
          { if (tag == off_tag)
              { for (j = 0; j < obytes/tsize; j++)
                  { OUTPUT_OFFSET(doff)
                    if (j < count)
                      { if (ctype == TIFF_SHORT)
                          doff += cntS[j];
                        else if (ctype == TIFF_LONG)
                          doff += cntI[j];
                        else
                          doff += cntO[j];
                      }
                  }
              }
            else if (tsize == 8)
              { if (tag->type == TIFF_DOUBLE || tag->type == TIFF_LONG64)
                  { uint64 *valD = (uint64 *) (ifd->values + tag->vector);
                    OUTPUT_QUAD(valD[0])
                  }
                else  // tag->type in { TIFF_RATIONAL, TIFF_SRATIONAL }
                  { uint32 *valI = (uint32 *) (ifd->values + tag->vector);
                    OUTPUT_LONG(valI[0])
                    OUTPUT_LONG(valI[1])
                  }
              }
            else if (tsize == 4)
              { uint32 *valI = (uint32 *) (ifd->values + tag->vector);
                for (j = 0; j < obytes/tsize; j++)
                  if (j < count)
                    OUTPUT_LONG(valI[j])
                  else
                    OUTPUT_LONG(0);
              }
            else if (tsize == 2)
              { uint16 *valS = (uint16 *) (ifd->values + tag->vector);
                for (j = 0; j < obytes/tsize; j++)
                  if (j < count)
                    OUTPUT_SHORT(valS[j])
                  else
                    OUTPUT_SHORT(0);
              }
            else
              { fwrite(ifd->values + tag->vector,(size_t) esize,1,output);
                if (esize < obytes)
                  fwrite(zeros,(size_t) (obytes-esize),1,output);
              }
          }
        else
          { OUTPUT_OFFSET(voff)
            voff += (uint64) esize;
          }
      }

    OUTPUT_OFFSET(tif->eof_offset)
  }

  //  Write IFD value vectors

  { uint32  j;
    uint64  doff;
    int     i;

    doff = data_offset;
    for (i = 0; i < ifd->numtags; i++)
      { Tif_Tag *tag   = ifd->tags+i;
        int      tsize = type_sizes[tag->type];
        int      esize = tsize * ((int) tag->count);

        if (tag->label == TIFF_JF_ANO_BLOCK && tif->ifd_no == 1)
          continue;

        if (tag == bps_tag)
          { uint16 *valS = (uint16 *) (ifd->values + tag->vector);
            OUTPUT_SHORT(valS[0])
            OUTPUT_SHORT(valS[1])
          }

        else if (tag == lsm_tag)
          { uint32 *valI = (uint32 *) (ifd->values + tag->vector);
            uint32  displace;
            int     i, idx, lflip;
        
            lflip = (valI[0] != 0x0300494C && valI[0] != 0x0400494C);

            if (lflip)
              for (i = 0; (idx = lsm_offset_list[i]) != 0; i++)
                flip_long(valI+idx);

            displace = (uint32) ftello(output);
            for (i = 0; (idx = lsm_offset_list[i]) != 0; i++)
              if (valI[idx] != 0)
                valI[idx] += displace;

            if (lflip)
              for (i = 0; (idx = lsm_offset_list[i]) != 0; i++)
                flip_long(valI+idx);

            fwrite(ifd->values + tag->vector,(size_t) esize,1,output); 

            if (lflip)
              for (i = 0; (idx = lsm_offset_list[i]) != 0; i++)
                flip_long(valI+idx);

            for (i = 0; (idx = lsm_offset_list[i]) != 0; i++)
              if (valI[idx] != 0)
                valI[idx] -= displace;

            if (lflip)
              for (i = 0; (idx = lsm_offset_list[i]) != 0; i++)
                flip_long(valI+idx);
          }

        else if (esize > obytes)

          { if (tag == off_tag)
              for (j = 0; j < tag->count; j++)
                { OUTPUT_OFFSET(doff)
                  if (ctype == TIFF_SHORT)
                    doff += cntS[j];
                  else if (ctype == TIFF_LONG)
                    doff += cntI[j];
                  else
                    doff += cntO[j];
                }

            else if (!flip || tsize == 1)
              fwrite(ifd->values + tag->vector,(size_t) esize,1,output); 

            else
              switch (tsize)
              { case 2:
                  { uint16 *valS = (uint16 *) (ifd->values + tag->vector);
                    for (j = 0; j < tag->count; j++)
                      OUTPUT_SHORT(valS[j])
                    break;
                  }
                case 4:
                  { uint32 *valI = (uint32 *) (ifd->values + tag->vector);
                    for (j = 0; j < tag->count; j++)
                      OUTPUT_LONG(valI[j])
                    break;
                  }
                case 8:
                  if (tag->type == TIFF_DOUBLE || tag->type == TIFF_LONG64)
                    { uint64 *valD = (uint64 *) (ifd->values + tag->vector);
                      for (j = 0; j < tag->count; j++)
                        OUTPUT_QUAD(valD[j]);
                    }
                  else  // tag->type in { TIFF_RATIONAL, TIFF_SRATIONAL }
                    { uint32 *valI = (uint32 *) (ifd->values + tag->vector);
                      for (j = 0; j < 2*tag->count; j++)
                        OUTPUT_LONG(valI[j])
                    }
                  break;
              }
          }
      }
  }

  if (ifd->source_ref == NULL)   //  If in memory then restore byte counts to uncompressed sizes
    Write_IFD_Image_8qm5(eifd,NULL);

  off_tag->type  = (uint16) otype;
  tif->ifd_no   += 1;
  tif->ifd_linko = block_offset + ((uint64) (block_size-obytes));

  fseeko(output,(off_t) tif->eof_offset,SEEK_SET);

  return (0);

error:
  if (my_message())
    sprintf(Tiff_Esource,"Write_Tiff_IFD");
  return (1);
}

static void close_tiff_writer(Twriter *tif)
{ FILE    *output;
  int      flip, flag;
  uint32   obytes;
  uint64   zero;

  if (tif->ifd_no == 1)
    write_tiff_header(tif,0);

  flip   = tif->flip;
  output = tif->output;
  flag   = tif->flag64;
  if (flag)
    obytes = 8;
  else
    obytes = 4;

  if (tif->ano_count > obytes)
    { fseeko(tif->output,(off_t) tif->eof_offset,SEEK_SET);
      fwrite(tif->annotation,tif->ano_count,1,tif->output);
      fseeko(tif->output,(off_t) tif->ano_linko,SEEK_SET);
      OUTPUT_OFFSET(tif->eof_offset)
    }

  fseeko(tif->output,(off_t) tif->ifd_linko,SEEK_SET);
  zero = 0;
  OUTPUT_OFFSET(zero)

  fclose(tif->output);
}

Tiff_IFD *Convert_2_RGB(Tiff_IFD *eifd, int source, int target)
{ static int Icode[2][2] = { { TIFF_STRIP_OFFSETS, TIFF_STRIP_BYTE_COUNTS },
                             { TIFF_TILE_OFFSETS,  TIFF_TILE_BYTE_COUNTS }
                           };

  TIFD *ifd = (TIFD *) eifd;

  void     *off, *cnt, *val;
  uint32   *valI;
  uint16   *spp;
  uint16   *valS, bps[3], get[3];
  Tiff_Type type, ctype, otype;
  int       count, hcount, size, idx, enc;

  type = TIFF_LONG;
  valI = (uint32 *) Get_Tiff_Tag(ifd,TIFF_NEW_SUB_FILE_TYPE,&type,&count);
  if (valI != NULL)
    if ((*valI & TIFF_VALUE_REDUCED_RESOLUTION) != 0)
      return (eifd);

  spp = (uint16 *) Get_Tiff_Tag(ifd,TIFF_SAMPLES_PER_PIXEL,&type,&count);
  if (spp == NULL) goto error;
  if (*spp == 1 || *spp == 3)
    return (eifd);
  if (*spp > 3)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"IFD contains more than 3 channels");
      goto error;
    }

  if (source >= 2 || target >= 3 || source < 0 || target < 0)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Source and target not in ranges 0..1 and 0..2 respectively");
      goto error;
    }
  get[0] = get[1] = get[2] = (uint16) (1-source);
  get[target] = (uint16) source;
    
  valS = (uint16 *) Get_Tiff_Tag(ifd,TIFF_BITS_PER_SAMPLE,&type,&count);
  if (valS == NULL) goto error;
  bps[0] = valS[get[0]];
  bps[1] = valS[get[1]];
  bps[2] = valS[get[2]];

  off = Get_Tiff_Tag(ifd,Icode[0][1],&otype,&count);
  enc = 0;
  if (off == NULL)
    { off = Get_Tiff_Tag(ifd,Icode[1][1],&otype,&count);
      enc = 1;
      if (off == NULL)
        { if (Grab_Message_8qm5())
           sprintf(Tiff_Estring,"No BYTE_COUNTS tag in IFD");
          goto error;
        }
    }
  hcount = count/2;
  
  cnt = Get_Tiff_Tag(ifd,Icode[enc][0],&ctype,&count);
  if (cnt == NULL) goto error;
  if (2*hcount != count)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Offset & byte_count tags do not have the same length");
      goto error;
    }

  if (Allocate_Tiff_Tag(ifd,TIFF_BITS_PER_SAMPLE,TIFF_SHORT,3) == NULL ||
      Allocate_Tiff_Tag(ifd,Icode[enc][1],otype,3*hcount) == NULL ||
      Allocate_Tiff_Tag(ifd,Icode[enc][0],ctype,3*hcount) == NULL)
    { Allocate_Tiff_Tag(ifd,Icode[enc][1],otype,2*hcount);
      Allocate_Tiff_Tag(ifd,TIFF_BITS_PER_SAMPLE,type,2);
      goto error;
    }

  *spp = 3;
  
  Set_Tiff_Tag(ifd,TIFF_BITS_PER_SAMPLE,TIFF_SHORT,3,bps);

  bps[0] = TIFF_VALUE_RGB; 
  Set_Tiff_Tag(ifd,TIFF_PHOTOMETRIC_INTERPRETATION,TIFF_SHORT,1,bps);

  val  = Get_Tiff_Tag(ifd,Icode[enc][1],&type,&count);
  size = hcount * type_sizes[type];
  for (idx = 0; idx <= 2; idx++)
    memcpy(((char *) val)+idx*size,((char *) off)+get[idx]*size,(size_t) size);

  val  = Get_Tiff_Tag(ifd,Icode[enc][0],&type,&count);
  size = hcount * type_sizes[type];
  for (idx = 0; idx <= 2; idx++)
    memcpy(((char *) val)+idx*size,((char *) cnt)+get[idx]*size,(size_t) size);

  return (eifd);

error:
  if (my_message())
    sprintf(Tiff_Esource,"Convert_2_RGB");
  return (NULL);
}

int Count_LSM_Colors(Tiff_IFD *eifd)
{ TIFD *ifd = (TIFD *) eifd;

  uint32   *lsmarr, *colarr, coloff;
  int       lflip, nchannels;

  lsmarr = (uint32 *) Get_Tiff_Tag(ifd,TIFF_CZ_LSM_INFO,NULL,NULL);
  if (lsmarr == NULL)
    { if (Grab_Message_8qm5())
        { sprintf(Tiff_Estring,"IFD does not contain LSM tag");
          sprintf(Tiff_Esource,"Count_LSM_Colors");
          return (0);
        }
    }
  
  lflip  = (lsmarr[0] != 0x0300494C && lsmarr[0] != 0x0400494C);

  coloff = lsmarr[LSM_CHANNEL_COLORS];
  if (lflip)
    flip_long(&coloff);
  colarr = (uint32 *) (((char *) lsmarr) + coloff);
  if (lflip)
    { flip_long(colarr+1);
      nchannels = (int) colarr[1];
      flip_long(colarr+1);
    }
  else
    nchannels = (int) colarr[1];
  return (nchannels);
}

int Get_LSM_Colors(Tiff_IFD *eifd, int nchannels, int *colors)
{ TIFD *ifd = (TIFD *) eifd;

  uint32   *lsmarr, *colarr, coloff, locoff;
  int       lflip, nchan, i;

  lsmarr = (uint32 *) Get_Tiff_Tag(ifd,TIFF_CZ_LSM_INFO,NULL,NULL);
  if (lsmarr == NULL)
    { if (Grab_Message_8qm5())
        { sprintf(Tiff_Estring,"IFD does not contain LSM tag");
          sprintf(Tiff_Esource,"Get_LSM_Colors");
          return (1);
        }
    }
  
  lflip  = (lsmarr[0] != 0x0300494C && lsmarr[0] != 0x0400494C);

  coloff = lsmarr[LSM_CHANNEL_COLORS];
  if (lflip)
    flip_long(&coloff);
  colarr = (uint32 *) (((char *) lsmarr) + coloff);
  if (lflip)
    { flip_long(colarr+1);
      nchan = (int) colarr[1];
      flip_long(colarr+1);
    }
  else
    nchan = (int) colarr[1];
  if (nchan != nchannels)
    { if (Grab_Message_8qm5())
        { sprintf(Tiff_Estring,"LSM does not have %d channels",nchannels);
          sprintf(Tiff_Esource,"Get_LSM_Colors");
          return (1);
        }
    }

  locoff = colarr[3];
  if (lflip)
    flip_long(&locoff);
  colarr = (uint32 *) (((char *) colarr) + locoff);

  for (i = 0; i < nchannels; i++)
    { colors[i] = (int) colarr[i];
      if (lflip)
        flip_long((uint32 *) (colors+i));
    }

  return (0);
}

/****************************************************************************************
 *                                                                                      *
 *  TIFF_ANNOTATOR ROUTINES                                                             *
 *                                                                                      *
 ****************************************************************************************/

static Tiff_Annotator *open_annotator(char *name, Annotator_Status *good)
{ int           mach_endian;
  struct stat   fdesc;

  Tannotator *tif;
  int         flag;
  uint64      offset;
  int         flip, obytes;
  uint16      order, ntags;
  int         infile;
  FILE       *input;
  int         isfetch;

  tif   = NULL;
  input = NULL;

  mach_endian = Native_Endian();

  if (good != NULL)
    { isfetch = 0;
      *good = ANNOTATOR_CANT_OPEN;
    }
  else
    isfetch = 1;

  input = fopen(name,"rb+");
  if (input == NULL)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Cannot open file %s for annotating",name);
      goto error;
    }
  infile = fileno(input);

  if (!isfetch)
    *good = ANNOTATOR_GIBBERISH;

  if (read(infile,&order,2) != 2)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"File ends prematurely");
      goto error;
    }
  if (order == 0x4949)
    flip = mach_endian;
  else if (order == 0x4D4D)
    flip = 1-mach_endian;
  else
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"File %s does not contain valid endian value",name);
      goto error;
    }

  if (read(infile,&order,2) != 2)
    { sprintf(Tiff_Estring,"File ends prematurely");
      goto error;
    }
  if (flip)
    flip_short(&order);

  flag   = 0;
  obytes = 4;
  if (order == 0x0040)
    { flag   = 1;
      obytes = 8;
    }
  else if (order != 0x002A)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"File %s does not contain a valid magic key",name);
      goto error;
    }

  if (flag)
    { if (read(infile,&offset,8) != 8)
        { if (Grab_Message_8qm5())
            sprintf(Tiff_Estring,"File ends prematurely");
          goto error;
        }
      if (flip)
        flip_quad(&offset);
    }
  else
    { uint32 offs;
      if (read(infile,&offs,4) != 4)
        { if (Grab_Message_8qm5())
            sprintf(Tiff_Estring,"File ends prematurely");
          goto error;
        }
      if (flip)
        flip_long(&offs);
      offset = offs;
    }

  if (offset == 0)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Trying to advance at end-of-ifd-list");
      goto error;
    }

  if (isfetch)
    { tif = new_tannotator(0,"Open_Tiff_Anotator");
      if (tif == NULL) goto error;

      tif->flip   = flip;
      tif->inout  = input;
      tif->flag64 = flag;
    }

  // Seek to the next IFD, see how many tags it has,
  //   and allocate the ifd record and its tag array

  if (offset != (uint32) (4+obytes))
    { if (lseek(infile,(off_t) offset,SEEK_SET) < 0)
        { if (Grab_Message_8qm5())
            sprintf(Tiff_Estring,"Seek for next IFD failed");
          goto error;
        }
    }
  if (read(infile,&ntags,2) != 2)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"File ends prematurely");
      goto error;
    }
  if (flip)
    flip_short(&ntags);

  // Read the tags, looking for ours and when find it setup additional fields of Tannotator

  { int i, tbytes;

    tbytes = 8+obytes;

    if (!isfetch)
      *good = ANNOTATOR_NOT_FORMATTED;

    for (i = 0; i < ntags; i++)

      { uint64 value = 0;
        uint16 label;
        int    count;
        void  *valptr;
        uint8  ifdtag[16];

        read(infile,ifdtag,(size_t) tbytes);

        label = *((uint16 *) ifdtag);
        if (flip)
          flip_short(&label);

        if (label == TIFF_JF_ANO_BLOCK)
          { if (!isfetch)
              *good = ANNOTATOR_NOT_FORMATTED;

            count = *((int *) (ifdtag+4));
            if (flip)
              flip_long(&count);

            fstat(infile,&fdesc);
            valptr = ifdtag+8;
            if (count > obytes)
              { if (flag)
                  { uint64 *valO = (uint64 *) valptr;
                    if (flip)
                      flip_quad(valO);
                    value = *valO;
                  }
                else
                  { uint32 *valI = (uint32 *) valptr;
                    if (flip)
                      flip_long(valI);
                    value = *valI;
                  }
                if (((int64) value) + count != fdesc.st_size)
                  { if (Grab_Message_8qm5())
                      sprintf(Tiff_Estring,"JF annotation block is not at end of file");
                    goto error;
                  }
              }

            if (isfetch)
              { if (allocate_tannotator_annotation(tif,count,"Open_Tiff_Annotator")) goto error;
                tif->ano_count  = (uint32) count;
                tif->ano_cnto   = offset + (uint64) (i + 6);
                if (count <= obytes)
                  { tif->ano_offset = (uint64) fdesc.st_size;
                    memcpy(tif->annotation,valptr,(size_t) count);
                  }
                else
                  { tif->ano_offset = (uint64) value;
                    lseek(infile,(off_t) value,SEEK_SET);    //  Must work as value < fdesc.st_size
                    read(infile,tif->annotation,(size_t) count);
                  }
              }

            break;
          }
      }
    if (i >= ntags)
      { if (Grab_Message_8qm5())
          sprintf(Tiff_Estring,"First IFD does not contain a JF annotation block");
        goto error;
      }
  }

  if (isfetch)
    return ((Tiff_Annotator *) tif);
  else
    { *good = ANNOTATOR_FORMATTED;
      close(infile);
      return ((Tiff_Annotator *) 1);
    }

error:
  if (tif != NULL)
    Free_Tiff_Annotator(tif);
  if (input != NULL)
    fclose(input);
  return (NULL);
}

Tiff_Annotator *Open_Tiff_Annotator(char *name)
{ Tiff_Annotator *ret;
  ret = open_annotator(name,NULL);
  if (ret == NULL)
    { if (my_message())
        sprintf(Tiff_Esource,"Open_Tiff_Annotator");
    }
  return (ret);
}

Annotator_Status Tiff_Annotation_Status(char *name)
{ Annotator_Status good;
  if (open_annotator(name,&good) == 0)
    { if (my_message())
        Tiff_Error_Release();
    }
  return (good);
}

char *Get_Tiff_Annotation(Tiff_Annotator *etif, int *count)
{ Tannotator *tif = (Tannotator *) etif;

  *count = (int) tif->ano_count;
  return (tif->annotation);
}

int Set_Tiff_Annotation(Tiff_Annotator *etif, char *anno, int count)
{ Tannotator *tif = (Tannotator *) etif;
 
  if (allocate_tannotator_annotation(tif,count,"Set_Tiff_Annotation"))
    { if (my_message())
        sprintf(Tiff_Esource,"Set_Tiff_Annotation");
      return (1);
    }
  tif->ano_count = (uint32) count;
  memcpy(tif->annotation,anno,(size_t) count);
  return (0);
}

static void close_tiff_annotator(Tannotator *tif)
{ FILE       *output;
  int         flip, flag;
  uint32      obytes;

  flip   = tif->flip;
  output = tif->inout;
  flag   = tif->flag64;
  if (flag)
    obytes = 8;
  else
    obytes = 4;

  optseek(tif->inout,(off_t) tif->ano_cnto,SEEK_SET);
  OUTPUT_LONG(tif->ano_count);

  if (tif->ano_count <= obytes)
    { fwrite(tif->annotation,tif->ano_count,1,tif->inout);
      ftruncate(fileno(tif->inout),(off_t) tif->ano_offset);
    }
  else
    { OUTPUT_OFFSET(tif->ano_offset);
      optseek(tif->inout,(off_t) tif->ano_offset,SEEK_SET);
      fwrite(tif->annotation,tif->ano_count,1,tif->inout);
      ftruncate(fileno(tif->inout),(off_t) (tif->ano_offset+tif->ano_count));
    }
}

int Format_Tiff_For_Annotation(char *name)
{ Tiff_Reader *rtif;
  Tiff_Writer *wtif;
  Tiff_IFD    *ifd;
  Tiff_Type    type;
  int          endian, flag64, lsm;
  int          i, count, mid;
  char        *template = "$mytiff.XXXXXX";
  char        *tempname;

  ifd      = NULL;
  wtif     = NULL;
  mid      = -1;
  tempname = NULL;
  rtif     = NULL;

  lsm = (strcmp(name+(strlen(name)-4),".lsm") == 0);

  rtif = Open_Tiff_Reader(name,&endian,&flag64,lsm);
  if (rtif == NULL) goto error;

  if (End_Of_Tiff(rtif))
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Empty tiff file");
      goto error;
    }

  tempname = (char *) Guarded_Realloc(NULL,strlen(template)+strlen(name)+2,NULL);
  if (tempname == NULL) goto error;

  strcpy(tempname,name);
  for (i = (int) strlen(name)-1; i >= 0; i--)
    if (tempname[i] == '/')
      break;
  strcpy(tempname+(i+1),template);
 
#ifndef _MSC_VER

  if ((mid = mkstemp(tempname)) < 0)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Could not create temporary file");
      goto error;
    }

  wtif = Open_Tiff_Writer(tempname,flag64,lsm);
  close(mid);
  if (wtif == NULL)
    { remove(tempname);
      goto error;
    }

#else

  (void) mid;
  mktemp(tempname);
  wtif = Open_Tiff_Writer(tempname,flag64,lsm);
  if (wtif == NULL)
    { if (Grab_Message_8qm5())
        sprintf(Tiff_Estring,"Could not create temporary file");
      goto error;
    }

#endif

  ifd = Read_Tiff_IFD(rtif);
  if (ifd == NULL) goto error;

  if (Get_Tiff_Tag(ifd,TIFF_JF_ANO_BLOCK,&type,&count) == NULL)
    if (Set_Tiff_Tag(ifd,TIFF_JF_ANO_BLOCK,TIFF_ASCII,1,"\0")) goto error;

  Write_Tiff_IFD(wtif,ifd);
  Free_Tiff_IFD(ifd);

  while ( ! End_Of_Tiff(rtif))
    { ifd = Read_Tiff_IFD(rtif);
      if (ifd == NULL) goto error;
      Write_Tiff_IFD(wtif,ifd);
      Free_Tiff_IFD(ifd);
    }

  Free_Tiff_Writer(wtif);
  close(mid);
  remove(name);
  rename(tempname,name);

  free(tempname);
  Free_Tiff_Reader(rtif);
  return (0);

error:
  if (ifd != NULL)
    Free_Tiff_IFD(ifd);
  if (wtif != NULL)
    { Free_Tiff_Writer(wtif);
      remove(tempname);
    }
  if (tempname != NULL)
    free(tempname);
  if (rtif != NULL)
    Free_Tiff_Reader(rtif);
  if (my_message())
    sprintf(Tiff_Esource,"Format_Tiff_For_Annotation");
  return (1);
}
