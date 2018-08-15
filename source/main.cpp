#include <ios>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <chrono>
#include <mutex>
#include <thread>
#include <memory>
#ifndef _WIN32
	#include <unistd.h>
#else
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <Windows.h>
#endif



namespace Bluebird
{
#ifdef _WIN32
	//Returns the last Win32 error, in string format. Returns an empty string if there is no error.
	const std::string GetLastErrorAsString(const DWORD& p_Error)
	{
		std::string l_result("No Error");
		
		if (p_Error != 0)
		{
			LPSTR messageBuffer(nullptr);
			size_t size(
				FormatMessageA(
					FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
					NULL,
					p_Error,
					MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
					(LPSTR)&messageBuffer,
					0,
					NULL));

			l_result = std::string(messageBuffer, size);

			//Free the buffer.
			LocalFree(messageBuffer);
		}

		return l_result;
	}
#endif

	struct Something
	{
		unsigned int X, Y, Z;
	};

	void PrintSomething(const Something& p_Other)
	{
		std::cout << "Record Print [0x" << std::hex << p_Other.X << ", 0x" << p_Other.Y << ", 0x" << p_Other.Z << "]" << std::endl;
	}

	using FileStreamPtr = std::shared_ptr<std::fstream>;

	// Type T must have a public default constructor, or a private
	// default constructor and "CumulativeWriter" defined as a friend
	template<typename T>
	class CumulativeWriter
	{
		public:

			enum class Status
			{
				Unknown,
				FileNotFound,
				ReadyClosed,
				ReadyOpen,
				Writing,
				Reading,
				Closing,
				Closed,
				ErrorOpeningStream,
				ErrorWriting,
				ErrorWritingStreamNotReady,
				ErrorSeeking,
				ErrorReading,
				PossibleCorruption,
				UnableToCalculateRecords
			};

			enum class RecordReadStatus
			{
				Unknown,
				OffsetOutOfRange,
				BadMemoryAlloc,
				StreamNotOpen,
				StreamReadError,
				Okay			=	255
			};

			enum class LoadState
			{
				Unknown,
				Corrupt		=	254,
				Okay		=	255
			};

		private:

			std::string		m_Filename;
#ifdef _WIN32
			HANDLE			m_FileStream;
			DWORD			m_FileSizeDword;
#else
			FileStreamPtr		m_FileStream;
#endif
			Status			m_Status;
			Status			m_PrevStatus;
			std::mutex		m_Lock;
			LoadState		m_LoadState;

			const unsigned int	c_RecordSize;
			unsigned int		m_RecordCount;

			CumulativeWriter() = delete;
			CumulativeWriter(const CumulativeWriter&) = delete;

		public:

			CumulativeWriter(const std::string& p_Filename)
				:
				m_Filename(p_Filename),
#ifdef _WIN32
				m_FileStream(INVALID_HANDLE_VALUE),
				m_FileSizeDword(0),
#else
				m_FileStream(nullptr),
#endif
				m_Status(Status::Unknown),
				m_PrevStatus(m_Status),
				m_Lock(),
				m_LoadState(LoadState::Unknown),
				m_RecordCount(0),
				c_RecordSize(sizeof(T))
			{
				m_Status = Status::ReadyClosed;
				OpenFileStream();
			}

			virtual ~CumulativeWriter()
			{
				Close();
			}

			inline const bool FileStreamValid() const noexcept
			{
#ifdef _WIN32
				return m_FileStream != INVALID_HANDLE_VALUE;
#else
				return m_FileStream != nullptr;
#endif
			}

		private:

			void OpenFileStream() noexcept
			{
				std::lock_guard<std::mutex> l_Lock(m_Lock);
				if (!FileStreamValid())
				{
					try
					{
#ifdef _WIN32
						m_FileStream = CreateFileA(
							m_Filename.c_str(),
							GENERIC_READ | GENERIC_WRITE,
							FILE_SHARE_READ | FILE_SHARE_WRITE,
							NULL,		// Default security
							OPEN_ALWAYS,// Open or create
							FILE_ATTRIBUTE_NORMAL, // | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
							NULL);
#else
						m_FileStream = std::make_shared<std::fstream>(
							m_Filename,
							std::ios_base::out | std::ios_base::in | std::ios_base::app | std::ios_base::ate);
						*m_FileStream << std::unitbuf;
#endif
						CalculateRecordCount();
						if (m_Status != Status::UnableToCalculateRecords)
						{
							m_Status = Status::ReadyOpen;
						}
					}
					catch (const std::exception&)
					{
						m_Status = Status::ErrorOpeningStream;
					}
				}
			}

			void CalculateRecordCount()
			{
				if (FileStreamValid())
				{
					try
					{
#ifdef _WIN32
						LARGE_INTEGER l_FileSize;
						if (GetFileSizeEx(m_FileStream, &l_FileSize) != 0)
						{
							unsigned int l_fpos(static_cast<unsigned int>(l_FileSize.QuadPart));
#else
							auto l_fpos(m_FileStream->tellg());
							//std::fpos_t l_fpos(l_Pos);
#endif
							m_RecordCount = static_cast<unsigned int>(l_fpos) / c_RecordSize;

							auto l_Remainder(l_fpos % c_RecordSize);
							if (l_Remainder == 0)
							{
								m_LoadState = LoadState::Okay;
							}
							else
							{
								//std::cout << "There are [" << l_fpos << "bytes] in the file which is [" << m_RecordCount << "] full records and [" << l_Remainder << "bytes] remaining" << std::endl;
								m_LoadState = LoadState::Corrupt;
							}							
#ifdef _WIN32
						}					
						else
						{
							m_LoadState = LoadState::Corrupt;
						}
#endif
					}
					catch (const std::exception&)
					{
						m_Status = Status::UnableToCalculateRecords;
					}
				}
			}

		public:

			using TPtr = std::shared_ptr<T>;
			using ReadRecordResult = std::pair<RecordReadStatus, TPtr>;

			const ReadRecordResult ReadRecord(const unsigned int& p_RecordOffset) noexcept
			{
				RecordReadStatus l_resultCode(RecordReadStatus::Unknown);
				std::shared_ptr<T> l_result(nullptr);

				if (FileStreamValid())
				{
					try
					{
						std::lock_guard<std::mutex> l_Lock(m_Lock);
						if (p_RecordOffset < m_RecordCount)
						{
							m_PrevStatus = m_Status;
							m_Status = Status::Reading;

							l_result = std::shared_ptr<T>(new T());
							
							try
							{
#ifdef _WIN32							
								LARGE_INTEGER l_NewFilePosition;
								LARGE_INTEGER l_SeekPos{ p_RecordOffset * c_RecordSize };
								if (SetFilePointerEx(
									m_FileStream,
									l_SeekPos,
									&l_NewFilePosition,
									FILE_BEGIN) != 0)
								{
									OVERLAPPED l_Overlapped{ 0 };
									if (ReadFileEx(
										m_FileStream,
										l_result.get(),
										c_RecordSize,
										&l_Overlapped,
										CompletionRoutine) != 0)
									{										
										m_Status = m_PrevStatus;
										l_resultCode = RecordReadStatus::Okay;
									}
									else
									{
										m_Status = Status::ErrorReading;
										throw std::exception("Read Error");
									}
								}
								else
								{
									m_Status = Status::ErrorSeeking;
									throw std::exception("Seeking Error");
								}
#else
								std::streampos l_SeekPos(p_RecordOffset * c_RecordSize);
								m_FileStream->seekg(l_SeekPos);
								m_FileStream->read(
									reinterpret_cast<char*>(l_result.get()),
									c_RecordSize);
								m_Status = m_PrevStatus;
								l_resultCode = RecordReadStatus::Okay;
#endif
							}
							catch (const std::exception&)
							{
								l_resultCode = RecordReadStatus::StreamReadError;
							}
						}
						else
						{
							l_resultCode = RecordReadStatus::OffsetOutOfRange;
						}
					}
					catch (const std::bad_alloc&)
					{
						l_resultCode = RecordReadStatus::BadMemoryAlloc;
					}
				}
				else
				{
					l_resultCode = RecordReadStatus::StreamNotOpen;
				}

				return std::make_pair(l_resultCode, l_result);
			}

			const ReadRecordResult LoadLastRecord() noexcept
			{
				return ReadRecord(m_RecordCount - 1);
			}

			const unsigned int& RecordCount() const noexcept
			{
				return m_RecordCount;
			}

			const unsigned int& RecordSize() const noexcept
			{
				return c_RecordSize;
			}

			const LoadState& LoadState() const noexcept
			{
				return m_LoadState;
			}

			const bool WasCorruptAtLoad() const noexcept
			{
				return m_LoadState == LoadState::Corrupt;
			}

			const bool WasOkayAtLoad() const noexcept
			{
				return m_LoadState == LoadState::Okay;
			}

			const bool Closing() const noexcept
			{
				return m_Status == Status::Closing || m_Status == Status::Closed;
			}

			void Close() noexcept
			{
				m_Status = Status::Closing;

				std::lock_guard<std::mutex> l_Lock(m_Lock);
				if (FileStreamValid())
				{
					try
					{
#ifdef _WIN32
						CloseHandle(m_FileStream);
						m_FileStream = INVALID_HANDLE_VALUE;
#else
						m_FileStream->close();
						m_FileStream = nullptr;
#endif
					}
					catch (const std::exception&)
					{
					}
				}

				m_Status = Status::Closed;
			}

#ifdef _WIN32
			static void WINAPI CompletionRoutine(DWORD u32_ErrorCode, DWORD u32_BytesTransfered, OVERLAPPED* pk_Overlapped)
			{
				// Purposefully empty function
			}
#endif

			const bool Write(const T* p_Record) noexcept
			{
				bool l_result(false);

				if (!Closing())
				{
					std::lock_guard<std::mutex> l_Lock(m_Lock);
					m_PrevStatus = m_Status;

					if (FileStreamValid())
					{
						m_Status = Status::Writing;
						try
						{
#ifdef _WIN32
							LARGE_INTEGER l_NewFilePointer;
							if (SetFilePointerEx(
								m_FileStream,
								{ 0 },
								&l_NewFilePointer,
								FILE_END) != 0)
							{
								OVERLAPPED l_Overlapped{ 0 };
								if (WriteFileEx(
									m_FileStream,
									p_Record,
									c_RecordSize,
									&l_Overlapped,
									CompletionRoutine) != 0)
								{
									if (FlushFileBuffers(m_FileStream) != 0)
									{
										int gothere = 1;
									}
									++m_RecordCount;
									m_Status = m_PrevStatus;
									l_result = true;
								}
								else
								{
									auto l_GLE(GetLastError());
									if (l_GLE != ERROR_IO_PENDING)
									{
										std::cout << GetLastErrorAsString(l_GLE).c_str() << std::endl;
										throw std::exception("Unable to Write");
									}
								}
							}
							else
							{
								auto l_GLE(GetLastError());
								if (l_GLE != ERROR_IO_PENDING)
								{
									std::cout << GetLastErrorAsString(l_GLE).c_str() << std::endl;
									throw std::exception("Unable to Seek");
								}
							}
#else
							*m_FileStream << std::unitbuf;
							m_FileStream->seekp(0, std::ios_base::end);
							m_FileStream->write(reinterpret_cast<const char*>(p_Record), c_RecordSize);
							m_FileStream->sync();
							sync();
							++m_RecordCount;
							m_Status = m_PrevStatus;
							l_result = true;
#endif														
						}
						catch (const std::exception&)
						{
							m_Status = Status::ErrorWriting;
						}
					}
					else
					{
						m_Status = Status::ErrorWritingStreamNotReady;
					}
				}

				return l_result;
			}
	};

}


int main()
{
	using namespace Bluebird;

	std::srand(static_cast<unsigned int>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
	unsigned int l_R1, l_R2, l_R3;

	unsigned int l_TotalTestCount(0);
	while (l_TotalTestCount < 500000)
	{
		std::cout << "\rLoad Test: " << ++l_TotalTestCount;

		CumulativeWriter<Something> l_File("test.txt");
		if (l_File.RecordCount() > 0)
		{
			if (l_File.WasCorruptAtLoad())
			{
				std::cout << "Corrupt At Load" << std::endl;
				break;
			}

			auto l_Loaded(l_File.LoadLastRecord());
			if (l_Loaded.first == Bluebird::CumulativeWriter<Something>::RecordReadStatus::Okay &&
				l_Loaded.second != nullptr)
			{
				if (l_TotalTestCount > 1)
				{
					bool l_Quit(false);
					if (l_R1 != l_Loaded.second->X)
					{
						std::cout << "X Loaded Wrong" << std::endl;
						l_Quit = true;
					}
					if (l_R2 != l_Loaded.second->Y)
					{
						std::cout << "Y Loaded Wrong" << std::endl;
						l_Quit = true;
					}
					if (l_R3 != l_Loaded.second->Z)
					{
						std::cout << "Z Loaded Wrong" << std::endl;
						l_Quit = true;
					}

					if (l_Quit)
					{
						Something l_Target{ l_R1, l_R2, l_R3 };
						std::cout << "Expected: ";
						PrintSomething(l_Target);
						std::cout << "Loaded: ";
						PrintSomething(*l_Loaded.second);
						break;
					}
				}
			}
			else
			{
				std::cout << "Load Error" << std::endl;
			}
		}

		l_R1 = std::rand();
		l_R2 = std::rand();
		l_R3 = std::rand();

		Something S{ l_R1, l_R2, l_R3 };
		l_File.Write(&S);
	}

	unsigned int l_WPS(0);
	l_TotalTestCount = 0;
	CumulativeWriter<Something> l_WriteFile("test2.txt");
	auto l_Time(std::chrono::steady_clock::now());
	while (l_TotalTestCount < 500000)
	{
		auto l_Duration(std::chrono::steady_clock::now() - l_Time);
		auto l_Cast(std::chrono::duration_cast<std::chrono::milliseconds>(l_Duration));
		if (l_Cast.count() >= 1000)
		{
			std::cout << "\rWPS: " << l_WPS << "               " << std::endl;
			l_Time = std::chrono::steady_clock::now();
			l_WPS = 0;
		}

		std::cout << "\rWrite Test: " << ++l_TotalTestCount;

		l_R1 = std::rand();
		l_R2 = std::rand();
		l_R3 = std::rand();

		Something S{ l_R1, l_R2, l_R3 };
		++l_WPS;
		l_WriteFile.Write(&S);
	}
	l_WriteFile.Close();

	Something l_Expected{ l_R1, l_R2, l_R3 };
	//std::cout << "Expected: ";
	//PrintSomething(l_Expected);

	CumulativeWriter<Something> l_ReadBack("test2.txt");
	if (l_ReadBack.WasOkayAtLoad())
	{
		//std::cout << "Loaded:   ";

		auto l_Loaded(l_ReadBack.LoadLastRecord());
		if (l_Loaded.first == Bluebird::CumulativeWriter<Something>::RecordReadStatus::Okay &&
			l_Loaded.second != nullptr)
		{
			if (l_Expected.X != l_Loaded.second->X)
			{
				std::cout << "X Failed..." << std::endl;
			}
			if (l_Expected.Y != l_Loaded.second->Y)
			{
				std::cout << "Y Failed..." << std::endl;
			}
			if (l_Expected.Z != l_Loaded.second->Z)
			{
				std::cout << "Z Failed..." << std::endl;
			}

			//PrintSomething(*l_Loaded.second);
		}
		else
		{
			std::cout << "Error" << std::endl;
		}
	}

	std::cout << std::endl;


	std::cout << "Enter an integer to quit..." << std::endl;
	int l_temp;
	std::cin >> l_temp;

}
