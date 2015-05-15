/*************************************************************************
*    UrBackup - Client/Server backup system
*    Copyright (C) 2011-2014 Martin Raiber
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#include "filesystem.h"
#include "../Interface/Server.h"
#include "../Interface/File.h"
#include "../stringtools.h"
#include <memory.h>
#ifdef _WIN32
#include <Windows.h>
#else
#include <errno.h>
#endif
#include "../Interface/Thread.h"
#include "../Interface/Condition.h"
#include "../Interface/ThreadPool.h"

namespace
{
	unsigned int getLastSystemError()
	{
		unsigned int last_error;
#ifdef _WIN32
		last_error=GetLastError();
#else
		last_error=errno;
#endif
		return last_error;
	}

	const size_t max_idle_buffers = 64;
	const size_t readahead_num_blocks = 5120;
	const size_t readahead_low_level_blocks = readahead_num_blocks/2;

	class ReadaheadThread : public IThread
	{
	public:
		ReadaheadThread(Filesystem& fs)
			: fs(fs), 
			  mutex(Server->createMutex()),
			  start_readahead_cond(Server->createCondition()),
			  read_block_cond(Server->createCondition()),
			  current_block(-1),
			  do_stop(false),
			  readahead_miss(false)
		{

		}

		~ReadaheadThread()
		{
			for(std::map<int64, char*>::iterator it=read_blocks.begin();
				it!=read_blocks.end();++it)
			{
				fs.releaseBuffer(it->second);
			}
		}


		void operator()()
		{
#ifdef _WIN32
#ifdef THREAD_MODE_BACKGROUND_BEGIN
			SetThreadPriority( GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#else
			SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif //THREAD_MODE_BACKGROUND_BEGIN
#endif

			IScopedLock lock(mutex.get());
			while(!do_stop)
			{
				if(read_blocks.size()>=readahead_num_blocks)
				{
					while(read_blocks.size()>readahead_low_level_blocks
						&& !readahead_miss)
					{
						start_readahead_cond->wait(&lock);

						if(do_stop) break;
					}
				}

				if(do_stop) break;

				while(current_block==-1)
				{
					start_readahead_cond->wait(&lock);
				}

				if(do_stop) break;

				std::map<int64, char*>::iterator it;
				do 
				{
					read_blocks.find(current_block);
					if(it!=read_blocks.end())
					{
						current_block = next_used_block(current_block);
					}
				} while (it!=read_blocks.end());

				if(current_block!=-1)
				{
					lock.relock(NULL);
					char* buf = fs.readBlock(current_block);
					lock.relock(mutex.get());
					read_blocks[current_block] = buf;

					if(readahead_miss)
					{
						read_block_cond->notify_all();
						readahead_miss=false;
					}
				}
			}

#ifdef _WIN32
#ifdef THREAD_MODE_BACKGROUND_END
			SetThreadPriority( GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
#else
			SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_NORMAL);
#endif
#endif
		}

		char* getBlock(int64 block)
		{
			IScopedLock lock(mutex.get());

			clearUnusedReadahead(block);

			char* ret=NULL;
			while(ret==NULL)
			{
				std::map<int64, char*>::iterator it=read_blocks.find(block);

				if(it!=read_blocks.end())
				{
					char* ret = it->second;
					read_blocks.erase(it);
				}
				else
				{
					readaheadFromInt(block);
					readahead_miss=true;
					read_block_cond->wait(&lock);
				}
			}

			return ret;
		}

		void stop()
		{
			IScopedLock lock(mutex.get());
			do_stop=true;
			start_readahead_cond->notify_all();
		}

	private:

		void readaheadFromInt(int64 pBlock)
		{
			current_block=pBlock;
			start_readahead_cond->notify_all();
		}

		void clearUnusedReadahead(int64 pBlock)
		{
			for(std::map<int64, char*>::iterator it=read_blocks.begin();
				it!=read_blocks.end();)
			{
				if(it->first<pBlock)
				{
					std::map<int64, char*>::iterator todel = it;
					++it;
					fs.releaseBuffer(todel->second);
					read_blocks.erase(todel);
				}
				else
				{
					break;
				}
			}
		}

		int64 next_used_block(int64 pBlock)
		{
			int64 size = fs.getSize();

			while(pBlock<size/fs.getBlocksize())
			{
				++pBlock;

				if(fs.hasBlock(pBlock))
				{
					return pBlock;
				}
			}

			return -1;
		}

		std::auto_ptr<IMutex> mutex;
		std::auto_ptr<ICondition> start_readahead_cond;
		std::auto_ptr<ICondition> read_block_cond;
		Filesystem& fs;

		std::map<int64, char*> read_blocks;

		bool readahead_miss;

		int64 current_block;

		bool do_stop;
	};
}

Filesystem::Filesystem(const std::wstring &pDev, bool read_ahead)
	: buffer_mutex(Server->createMutex())
{
	has_error=false;

	dev=Server->openFile(pDev, MODE_READ_DEVICE);
	if(dev==NULL)
	{
		Server->Log("Error opening device file. Errorcode: "+nconvert(getLastSystemError()), LL_ERROR);
		has_error=true;
	}
	own_dev=true;

	if(read_ahead)
	{
		readahead_thread.reset(new ReadaheadThread(*this));
		readahead_thread_ticket = Server->getThreadPool()->execute(readahead_thread.get());
	}
}

Filesystem::Filesystem(IFile *pDev, bool read_ahead)
	: dev(pDev)
{
	has_error=false;
	own_dev=false;

	if(read_ahead)
	{
		readahead_thread.reset(new ReadaheadThread(*this));
		readahead_thread_ticket = Server->getThreadPool()->execute(readahead_thread.get());
	}
}

Filesystem::~Filesystem()
{
	if(readahead_thread.get()!=NULL)
	{
		readahead_thread->stop();
		Server->getThreadPool()->waitFor(readahead_thread_ticket);
		readahead_thread.reset();
	}

	if(dev!=NULL && own_dev)
	{
		Server->destroy(dev);
	}
	
	for(size_t i=0;i<buffers.size();++i)
	{
		delete[] buffers[i];
	}
}

bool Filesystem::hasBlock(int64 pBlock)
{
	const unsigned char *bitmap=getBitmap();
	int64 blocksize=getBlocksize();

	size_t bitmap_byte=(size_t)(pBlock/8);
	size_t bitmap_bit=pBlock%8;

	unsigned char b=bitmap[bitmap_byte];

	bool has_bit=((b & (1<<bitmap_bit))>0);

	return has_bit;
}

char* Filesystem::readBlock(int64 pBlock)
{
	const unsigned char *bitmap=getBitmap();
	int64 blocksize=getBlocksize();

	size_t bitmap_byte=(size_t)(pBlock/8);
	size_t bitmap_bit=pBlock%8;

	unsigned char b=bitmap[bitmap_byte];

	bool has_bit=((b & (1<<bitmap_bit))>0);

	if(!has_bit)
		return NULL;
	
	if(readahead_thread.get()==NULL)
	{
		bool b=dev->Seek(pBlock*blocksize);
		if(!b)
		{
			Server->Log("Seeking in device failed -1", LL_ERROR);
			has_error=true;
			return NULL;
		}
		char* buf = getBuffer();
		if(!readFromDev(buf, (_u32)blocksize) )
		{
			Server->Log("Reading from device failed -1", LL_ERROR);
			has_error=true;
			return NULL;
		}

		return buf;
	}
	else
	{
		return readahead_thread->getBlock(pBlock);
	}
}

std::vector<int64> Filesystem::readBlocks(int64 pStartBlock, unsigned int n,
	const std::vector<char*>& buffers, unsigned int buffer_offset)
{
	_u32 blocksize=(_u32)getBlocksize();
	std::vector<int64> ret;

	size_t currbuf = 0;

	for(int64 i=pStartBlock;i<pStartBlock+n;++i)
	{
		char* buf = readBlock(i);
		if(buf!=NULL)
		{
			memcpy(buffers[currbuf]+buffer_offset, buf, blocksize);
			++currbuf;
			ret.push_back(i);

			releaseBuffer(buf);
		}
	}

	return ret;
}

bool Filesystem::readFromDev(char *buf, _u32 bsize)
{
	int tries=20;
	_u32 rc=dev->Read(buf, bsize);
	while(rc<bsize)
	{
		Server->wait(200);
		Server->Log("Reading from device failed. Retrying. Errorcode: "+nconvert(getLastSystemError()), LL_WARNING);
		rc+=dev->Read(buf+rc, bsize-rc);
		--tries;
		if(tries<0)
		{
			Server->Log("Reading from device failed. Errorcode: "+nconvert(getLastSystemError()), LL_ERROR);
			return false;
		}
	}
	return true;
}

int64 Filesystem::calculateUsedSpace(void)
{
	const unsigned char *bm=getBitmap();
	uint64 blocks1=getSize()/getBlocksize();
	unsigned int tadd=(unsigned int)(blocks1/8);;
	if( blocks1%8>0)
		++tadd;

	const unsigned char *target=bm+tadd;
	int64 used_blocks=0;
	uint64 blocknum=0;
	while(bm!=target)
	{
		const unsigned char b=*bm;
		for(int i=0;i<8;++i)
		{
			if(blocknum>=blocks1)
				break;
			if( (b & (1<<i))>0 )
			{
				++used_blocks;
			}
			++blocknum;
		}
		++bm;
	}
	return used_blocks*getBlocksize();
}

bool Filesystem::hasError(void)
{
	return has_error;
}

char* Filesystem::getBuffer()
{
	{
		IScopedLock lock(buffer_mutex.get());

		if(!buffers.empty())
		{
			char* ret = buffers[buffers.size()-1];
			buffers.erase(buffers.begin()+buffers.size()-1);
			return ret;
		}
	}

	return new char[getBlocksize()];
}

void Filesystem::releaseBuffer(char* buf)
{
	{
		IScopedLock lock(buffer_mutex.get());
		
		if(buffers.size()<max_idle_buffers)
		{
			buffers.push_back(buf);
			return;
		}
	}

	delete[] buf;
}

