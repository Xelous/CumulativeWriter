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
				ReadyClosed,
				ReadyOpen,
				Writing,
				Reading,
				Closing,
				Closed,
				ErrorOpeningStream,
				ErrorWriting,
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
			FileStreamPtr		m_FileStream;
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
				m_FileStream(nullptr),
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

		private:

			void OpenFileStream() noexcept
			{
				std::lock_guard<std::mutex> l_Lock(m_Lock);
				if (!m_FileStream)
				{
					try
					{
						m_FileStream = std::make_shared<std::fstream>(
							m_Filename,
							std::ios_base::out | std::ios_base::in | std::ios_base::app | std::ios_base::ate);
						*m_FileStream << std::unitbuf;
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
				if (m_FileStream)
				{
					try
					{
						auto l_Pos(m_FileStream->tellg());
						std::fpos_t l_fpos(l_Pos);

						m_RecordCount = static_cast<unsigned int>(l_fpos) / c_RecordSize;

						auto l_Remainder(l_fpos % c_RecordSize);
						if ( l_Remainder == 0 )
						{
							m_LoadState = LoadState::Okay;
						}
						else
						{
							std::cout << "There are [" << l_fpos << "bytes] in the file which is [" << m_RecordCount << "] full records and [" << l_Remainder << "bytes] remaining" << std::endl;

							m_LoadState = LoadState::Corrupt;
						}

						m_FileStream->seekg(0, std::ios_base::beg);
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

				if (m_FileStream)
				{
					try
					{
						std::lock_guard<std::mutex> l_Lock(m_Lock);
						if (p_RecordOffset < m_RecordCount)
						{
							m_PrevStatus = m_Status;
							m_Status = Status::Reading;

							l_result = std::shared_ptr<T>(new T());

							std::streampos l_SeekPos(p_RecordOffset * c_RecordSize);
							try
							{
								m_FileStream->seekg(l_SeekPos);
								m_FileStream->read(
									reinterpret_cast<char*>(l_result.get()),
									c_RecordSize);

								m_Status = m_PrevStatus;

								l_resultCode = RecordReadStatus::Okay;
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
				return m_RecordSize;
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
				if (m_FileStream)
				{
					try
					{
						m_FileStream->close();
						m_FileStream = nullptr;
					}
					catch (const std::exception&)
					{
					}
				}

				m_Status = Status::Closed;
			}

			const bool Write(const T* p_Record) noexcept
			{
				bool l_result(false);

				if (!Closing())
				{
					std::lock_guard<std::mutex> l_Lock(m_Lock);
					m_PrevStatus = m_Status;

					if (m_FileStream)
					{
						m_Status = Status::Writing;
						try
						{
							m_FileStream->seekp(0, std::ios_base::end);
							m_FileStream->write(reinterpret_cast<const char*>(p_Record), c_RecordSize);
							m_FileStream->sync();
#ifndef _WIN32
							sync();
#else
#endif
							std::this_thread::sleep_for(std::chrono::milliseconds(50));
							++m_RecordCount;
							m_Status = m_PrevStatus;
							l_result = true;
						}
						catch (const std::exception&)
						{
							m_Status = Status::ErrorWriting;
						}
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
	while (l_TotalTestCount < 10000)
	{
		++l_TotalTestCount;

		std::cout << "Test: " << l_TotalTestCount << std::endl;

		CumulativeWriter<Something> l_File("d:\\test.txt");
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
		}

		l_R1 = std::rand();
		l_R2 = std::rand();
		l_R3 = std::rand();

		Something S{ l_R1, l_R2, l_R3 };
		l_File.Write(&S);
	}

	std::cout << "Enter an integer to quit..." << std::endl;
	int l_temp;
	std::cin >> l_temp;

}
