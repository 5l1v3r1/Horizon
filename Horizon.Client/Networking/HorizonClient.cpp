#include "HorizonClient.h"
#include <iostream>

namespace Horizon::Client::Networking
{
	void HorizonClient::OnDataReceived(CSocketHandle*, const BYTE*, DWORD, const SockAddrIn&)
	{
		std::cout << "Hello World";
	}

	void HorizonClient::OnConnectionDropped(CSocketHandle*)
	{
		std::cout << "Connection Dropped";
	}
}
