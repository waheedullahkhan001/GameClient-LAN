#pragma once
// Stand-in for the engine CRC: any stable checksum works, both ends of the test use the same one.
class CRC
{
public:
	void computeCRC(const void* data, Int len)
	{
		const UnsignedByte* p = (const UnsignedByte*)data;
		for (Int i = 0; i < len; ++i)
		{
			m_crc = (m_crc << 5) + m_crc + p[i];
		}
	}
	UnsignedInt get() const { return m_crc; }
private:
	UnsignedInt m_crc = 5381;
};
