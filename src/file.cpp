#include "file.h"

#include <sstream>
#include <sys/mman.h>
#include <syslog.h>
#include <cassert>

#include "gridfs_fuse.h"

#include "lock.h"

namespace gridfs {

  File::File(const std::string& aPath):
    FilesystemEntry(aPath),
    theFileLength(0),
    theChunkSize(0),
    theWritten(0),
    theCurrentDataSize(0),
    theData(0),
    theHasChanges(false),
    theCachedChunkN(-1),
    theCachedChunk(mongo::BSONObj())
  {
    pthread_mutex_init(&mutex_read, NULL);
  }     

  File::~File()
  {
    free_memory();
    pthread_mutex_destroy(&mutex_read);
  }

  size_t
  File::write(const char * data, size_t size,off_t offset)
  {
    // lazy init
    if (theChunkSize==0)
      theChunkSize=FUSE.config.mongo_chunk_size;

    // disallow appending to a file
    // appending doesn't fit to the mongo gridfs modell
    if(theWritten != (size_t)offset){
      std::stringstream lMsg;
      lMsg << "Appending to a file other than to the beginning of it is not allowed. "
           << "\n    Mongo's Gridfs doesn't allow appending either. "
           << "\n    Instead you should replace the complete file. "
           << "\n    You tried to append to file " << path() << " with offset " << offset;
      throw std::runtime_error(lMsg.str());
    }

    // make sure we have enough virtual memory
    if(theCurrentDataSize==0)
    {
      syslog(LOG_DEBUG,
          "requesting virtual memory. size %i offset %i chunksize %i",
          (int) size, (int) offset, (int) theChunkSize);
      init_memory();
    }

    while(theCurrentDataSize < (offset + size))
    {
      syslog(LOG_DEBUG,
          "requesting extra virtual memory. size %i offset %i chunksize %i",
          (int) size, (int) offset, (int) theChunkSize);
      extend_memory();
    }
      
    memcpy( 
      ((char*)theData) + offset /* destination mem pointer */, 
      data /* source mem pointer */, 
      size);
    theHasChanges = true;
    theWritten += size;
    return size;
  }

  void 
  File::store()
  {
    gridfs().storeFile((const char*)theData, theWritten, path(), gridfile().getContentType());
    free_memory(); // clean dirty flag and release virtual memory
    theFileLength = theWritten;
    
    synchonizeUpdate();
  }

  size_t
  File::read(char *data, size_t size, off_t offset)
  {
    if (theChunkSize == 0)
      theChunkSize = gridfile().getChunkSize();

    if (theFileLength == 0)
      theFileLength = gridfile().getContentLength();

    if (theFileLength < (size_t)offset)
    {
      syslog(LOG_INFO, "read out of range; path: %s, offset: %i, size: %i, filesize: %i",
          path().c_str(), (int)offset, (int)size, (int)theFileLength);
      return 0;
    }

    size_t read = 0; // bytes read
    while (read != size && (offset+read) < theFileLength)
    {
      int chunkN = (int) (offset+read) / (int) theChunkSize;
      size_t aligned_size = (offset+size > theFileLength)?(theFileLength - offset):size; // align size to filelength
      aligned_size -= read; // substract what we already read
      size_t chunk_offset = (int) (offset+read) % (int) theChunkSize; 

      // align size to chunksize
      if ( (aligned_size + chunk_offset) > theChunkSize) 
        aligned_size = theChunkSize - chunk_offset; 

      // put data from chunk to the right position in buffer
      read += this->read(chunkN, data + read, aligned_size, chunk_offset); 
    }

    return read;
  }

  void
  File::truncate()
  {
    // create update filter and query
    mongo::BSONObj filter = BSON("_id" << gridfile().getFileField("_id").OID());
    mongo::BSONObj update = BSON( "$set" 
                               << BSON ( "length" << 0 ));

    // TODO DK this is not multi process safe because it updates an existing entry and 
    // doesn't store a new file entry, but don't see a better solution yet.
    // As an alternative, you could read the stat first and then create a new file but 
    // this is also not multi process safe (race condition between 2 steps) 
    theConnection->update(filesCollection(), filter, update);

    synchonizeUpdate();
  }

  size_t
  File::read(int chunkN, char *data, size_t size, off_t offset)
  {
    // this function must be synchonized
    // otherwise cached chunks can swap while being read
    // the scoped lock will be released even if an exception is thrown
    gridfs::Lock scopedLock(mutex_read);

    // this funkction must only be called with reads aligned to fit into one chunk
    assert(theChunkSize);
    assert(offset + size <= theChunkSize);
    assert(offset + size <= theFileLength);

    // see if we have the right chunk in cache. Fetch it if not.
    if(chunkN != theCachedChunkN){ 
      theCachedChunk = gridfile().getChunk(chunkN);
      theCachedChunkN = chunkN;
      syslog(LOG_DEBUG, "fetched chunk %i into cache of file %s",
          chunkN, path().c_str());
    }

    // fill buffer as requested
    int len;
    const char* chunk_data = theCachedChunk.data(len);
    memcpy(data, chunk_data + offset, size);

    return size;
  }

  void
  File::init_memory()
  {
    theData = mmap(NULL /* let kernel choose address */, 
                   theChunkSize, 
                   PROT_WRITE,
                   MAP_PRIVATE /* not shared between processes */
#                  ifdef __APPLE__
                     | MAP_ANON /* not backed by a real file */,
#                  else
                     | MAP_ANONYMOUS /* not backed by a real file */,
#                  endif
                   -1 /* invalid file descriptor */, 
                   0 /* offset ignored anyway with invalid file descriptor */); 

    // check if anything went wrong
    if (theData==MAP_FAILED)
    {
      std::stringstream lMsg;
      lMsg
        << "Allocating virtual memory by the kernel failed."
        << " Cannot write to file " << path();
      syslog(LOG_ERR, "%s", lMsg.str().c_str());
      throw std::runtime_error(lMsg.str());
    }

    // successfully initialized
    theCurrentDataSize=theChunkSize;
  }

  void
  File::extend_memory()
  {
    theData = mremap(theData /* old address */,
                     theCurrentDataSize,
                     (theCurrentDataSize + theChunkSize) /* new extended size */,
                     MREMAP_MAYMOVE /* kernel is permitted to relocate the mapping to a new virtual address */);

    // check if anything went wrong
    if (theData==MAP_FAILED)
    {
      std::stringstream lMsg;
      lMsg
        << "Extending virtual memory by the kernel failed."
        << " Cannot write to file " << path();
      syslog(LOG_ERR, "%s", lMsg.str().c_str());
      throw std::runtime_error(lMsg.str());
    }
    
    // successfully extended virtual memory
    theCurrentDataSize+=theChunkSize;
  }

  void
  File::free_memory()
  {
    if (theCurrentDataSize!=0)
    {
      munmap(theData,theCurrentDataSize);
      theWritten = 0;
      theHasChanges = false; 
      theCurrentDataSize = 0;
    }
  }

}
