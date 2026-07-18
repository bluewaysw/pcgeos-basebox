#define CPP_MODULE

#include "cpu/cpu.h"
#include "dosbox.h"
#include "gui/common.h"
#include "hardware/input/mouse.h"
#include "hardware/timer.h"

#include "ints/int10.h"
#include <SDL.h>

#include "SDL3_net/SDL_net.h"

#include "tls_root_ca.h"
#include "tlse.h"

#if C_GEOSHOST

#define GEOSHOST_DEBUG 0

#if GEOSHOST_DEBUG
#define GH_DBG(fmt, ...) LOG_MSG("GEOSHOST: " fmt, ##__VA_ARGS__)
#else
#define GH_DBG(fmt, ...) ((void)0)
#endif

#define MAX_ASYNC_OP_SLOTS 16

#define HIF_API_HOST   0
#define HIF_API_VIDEO  1
#define HIF_API_SSL    2
#define HIF_API_SOCKET 3

#define HIF_SLOT_AX 0
#define HIF_SLOT_SI 1
#define HIF_SLOT_BX 2
#define HIF_SLOT_CX 3
#define HIF_SLOT_DX 4
#define HIF_SLOT_DI 5

#define HIF_NOTIFY_DISPLAY_SIZE_CHANGE 1
#define HIF_NOTIFY_SOCKET_STATE_CHANGE 2

#define HIF_CHECK_API           98
#define HIF_SET_VIDEO_PARAMS    4
#define HIF_SET_EVENT_INTERRUPT 5
#define HIF_EVENT_ASYNC_END     7
#define HIF_GET_VIDEO_PARAMS    8
#define HIF_GET_EVENT           9

#define HIF_OK                 0
#define HIF_NOT_FOUND          1
#define HIF_NO_MEMORY          2
#define HIF_ASYNC_OP           3
#define HIF_TABLE_FULL         4
#define HIF_PENDING            5
#define HIF_EVENT_NOTIFICATION 6
#define HIF_FAILED             7

#define HIF_NETWORKING_BASE     1000
#define HIF_NC_RESOLVE_ADDR     HIF_NETWORKING_BASE
#define HIF_NC_ALLOC_CONNECTION HIF_NETWORKING_BASE + 1
#define HIF_NC_CONNECT_REQUEST  HIF_NETWORKING_BASE + 2
#define HIF_NC_SEND_DATA        HIF_NETWORKING_BASE + 3
#define HIF_NC_NEXT_RECV_SIZE   HIF_NETWORKING_BASE + 4
#define HIF_NC_RECV_NEXT        HIF_NETWORKING_BASE + 5
#define HIF_NC_RECV_NEXT_CLOSE  HIF_NETWORKING_BASE + 6
#define HIF_NC_CLOSE            HIF_NETWORKING_BASE + 7
#define HIF_NC_DISCONNECT       HIF_NETWORKING_BASE + 8
#define HIF_NC_CONNECTED        HIF_NETWORKING_BASE + 9
#define HIF_NETWORKING_END      1199

#define HIF_SSL_BASE                 1200
#define HIF_SSL_V2_GET_CLIENT_METHOD HIF_SSL_BASE
#define HIF_SSL_SSLEAY_ADD_SSL_ALGO  HIF_SSL_BASE + 1
#define HIF_SSL_CTX_NEW              HIF_SSL_BASE + 2
#define HIF_SSL_CTX_FREE             HIF_SSL_BASE + 3
#define HIF_SSL_NEW                  HIF_SSL_BASE + 4
#define HIF_SSL_FREE                 HIF_SSL_BASE + 5
#define HIF_SSL_SET_FD               HIF_SSL_BASE + 6
#define HIF_SSL_CONNECT              HIF_SSL_BASE + 7
#define HIF_SSL_SHUTDOWN             HIF_SSL_BASE + 8
#define HIF_SSL_READ                 HIF_SSL_BASE + 9
#define HIF_SSL_WRITE                HIF_SSL_BASE + 10
#define HIF_SSL_V23_CLIENT_METHOD    HIF_SSL_BASE + 11
#define HIF_SSL_V3_CLIENT_METHOD     HIF_SSL_BASE + 12
#define HIF_SSL_GET_SSL_METHOD       HIF_SSL_BASE + 13
#define HIF_SSL_SET_CALLBACK         HIF_SSL_BASE + 14
#define HIF_SSL_SET_TLSEXT_HOST_NAME HIF_SSL_BASE + 15
#define HIF_SSL_SET_SSL_METHOD       HIF_SSL_BASE + 16
#define HIF_SSL_END                  1299

const static char G_baseboxID[]  = "XOBESAB2";
static uint8_t G_baseboxIDOffset = 1;

static uint8_t G_commandOffset = 0;
static uint16_t G_commandBuffer[6];
static uint8_t G_responseOffset = 0;
static uint16_t G_responseBuffer[6];
static uint8_t G_eventInterrupt = 0;
SDL_mutex* G_eventQueueMutex    = NULL;
bool G_recheckEventInterrupt    = false;
bool G_protectedOpMode          = false;
static uint16_t G_nextAsyncID   = 1;

static int G_lookupNext = 1;

class EventRecord {
private:
	volatile uint16_t m_Payload[6];
	EventRecord* m_Next;

public:
	EventRecord(uint16_t* eventRecord, EventRecord* next) : m_Next(next)
	{
		memcpy((void*)m_Payload, eventRecord, sizeof(m_Payload));
	}

	void SetNext(EventRecord* nextEvent)
	{
		m_Next = nextEvent;
	}
	EventRecord* GetNext()
	{
		return m_Next;
	}
	void GetRecordData(uint16_t* recordBuf)
	{
		memcpy(recordBuf, (const void*)m_Payload, sizeof(m_Payload));
	}
};

#define UNALLOC_ASYNC_OP_ID 0

class AsyncOp {
private:
	AsyncOp* m_Next;
	uint16_t m_ID;
	SDL_Thread* m_thread;
	bool m_EventSent;

protected:
	bool m_RunOwnThread;
	uint16_t m_Result[6];

public:
	AsyncOp(AsyncOp* next)
	{
		m_Next         = next;
		m_Result[0]    = HIF_PENDING;
		m_thread       = NULL;
		m_EventSent    = false;
		m_RunOwnThread = true;
	}
	virtual ~AsyncOp() {};

	virtual uint16_t Init(uint16_t* cmdRec);
	AsyncOp* GetNext()
	{
		return m_Next;
	}
	uint16_t GetID()
	{
		return m_ID;
	}
	AsyncOp* Cleanup();

	virtual uint16_t RunAsync()
	{
		return 0;
	};
	virtual uint16_t PollStatus()
	{
		return 0;
	};
	void HandleCompletion();
};

class AsyncSocketResolveAddr : public AsyncOp {
private:
	char m_hostname[256];
	NET_Address* m_Addr;

public:
	AsyncSocketResolveAddr(AsyncOp* next) : AsyncOp(next)
	{
		m_Addr = NULL;
	};
	~AsyncSocketResolveAddr() {};

	uint16_t Init(uint16_t* cmdRec);
	uint16_t RunAsync();
	uint16_t PollStatus();
};

class AsyncSocketConnect : public AsyncOp {
private:
	enum State { IDLE, RESOLVING, CONNECTING, CONNECTED, DONE };
	uint16_t m_socketHandle;
	NET_Address* m_Addr;
	Uint16 m_Port;
	State m_State;

public:
	AsyncSocketConnect(AsyncOp* next) : AsyncOp(next)
	{
		m_Addr  = NULL;
		m_State = IDLE;
	};
	~AsyncSocketConnect() {};

	uint16_t Init(uint16_t* cmdRec);
	uint16_t RunAsync();
	uint16_t PollStatus();
};

class AsyncSSLConnect : public AsyncOp {
private:
	enum State { IDLE, SENDING, RECEIVING, DONE };
	State m_State;
	struct TLSContext* m_ctx;
	int m_socketHandle;
	uint8_t m_Buffer[8192];

public:
	AsyncSSLConnect(AsyncOp* next) : AsyncOp(next)
	{
		m_State        = IDLE;
		m_socketHandle = 0;
		m_ctx          = NULL;
	};
	~AsyncSSLConnect() {};

	uint16_t Init(uint16_t* cmdRec);
	uint16_t RunAsync();
	uint16_t PollStatus();
};

class AsyncSSLWrite : public AsyncOp {
private:
	struct TLSContext* m_ctx;
	int m_socketHandle;
	uint8_t m_Buffer[8192];
	int m_written;

public:
	AsyncSSLWrite(AsyncOp* next) : AsyncOp(next)
	{
		m_socketHandle = 0;
		m_ctx          = NULL;
	};
	~AsyncSSLWrite() {};

	uint16_t Init(uint16_t* cmdRec);
	uint16_t RunAsync();
	uint16_t PollStatus();
};

class AsyncSSLRead : public AsyncOp {
private:
	struct TLSContext* m_ctx;
	int m_socketHandle;
	uint8_t m_Buffer[8192];
	uint16_t m_BufferSegment;
	uint16_t m_BufferOffset;
	uint16_t m_BufferSize;

public:
	AsyncSSLRead(AsyncOp* next) : AsyncOp(next)
	{
		m_socketHandle = 0;
		m_ctx          = NULL;
	};
	~AsyncSSLRead() {};

	uint16_t Init(uint16_t* cmdRec);
	uint16_t RunAsync();
	uint16_t PollStatus();
};

class AsyncSocketSend : public AsyncOp {
private:
	uint16_t m_socketHandle;

public:
	AsyncSocketSend(AsyncOp* next) : AsyncOp(next) {};
	~AsyncSocketSend() {};

	uint16_t Init(uint16_t* cmdRec);
	uint16_t RunAsync();
	uint16_t PollStatus();
};

struct SocketState {
	volatile bool used;
	volatile bool open;
	volatile bool blocking;
	// NET_StreamSocket socketSet;
	NET_StreamSocket* stream;
	char* recvBuf;
	volatile int recvBufUsed;
	volatile bool receiveDone;
	volatile bool sendDone;
	volatile bool done;
	volatile bool ssl;
	volatile bool sslInitialEnd;

	SocketState()
	        : used(false),
	          recvBuf(NULL),
	          recvBufUsed(0),
	          receiveDone(false),
	          sendDone(false),
	          done(false),
	          ssl(false),
	          sslInitialEnd(false)
	{}
};

static const int MaxSockets = 256;

static SocketState NetSockets[MaxSockets];

EventRecord* G_eventRecords = NULL;
AsyncOp* G_opRecords        = NULL;

void GeosHost_NotifySocketChange();

static uint32_t ResolveAddress(uint16_t segment, uint16_t offset)
{
	if (G_protectedOpMode) {
		Descriptor desc;
		cpu.gdt.GetDescriptor(segment, desc);
		return static_cast<uint32_t>(desc.GetBase()) + offset;
	} else {
		return static_cast<uint32_t>(segment << 4) + offset;
	}
}

static void ReadDosMemory(uint16_t segment, uint16_t offset, void* dest, int size)
{
	uint32_t addr = ResolveAddress(segment, offset);
	uint8_t* buf  = static_cast<uint8_t*>(dest);
	for (int i = 0; i < size; i++) {
		buf[i] = mem_readb(addr + i);
	}
}

static void WriteDosMemory(uint16_t segment, uint16_t offset, const void* src, int size)
{
	uint32_t addr      = ResolveAddress(segment, offset);
	const uint8_t* buf = static_cast<const uint8_t*>(src);
	for (int i = 0; i < size; i++) {
		mem_writeb(addr + i, buf[i]);
	}
}

// ============================================================
// Sub-API framework
// ============================================================

class GeosHostSubAPI {
public:
	virtual ~GeosHostSubAPI() {}
	virtual uint8_t GetApiID() const                = 0;
	virtual uint16_t GetVersion() const             = 0;
	virtual bool HandlesCommand(uint16_t cmd) const = 0;
	virtual void HandleCommand(uint16_t cmd)        = 0;

	// For overlap checking at registration
	virtual int GetCommandList(const uint16_t** outList) const
	{
		*outList = nullptr;
		return 0;
	}
	virtual bool GetCommandRange(uint16_t& min, uint16_t& max) const
	{
		return false;
	}
};

class RangeSubAPI : public GeosHostSubAPI {
protected:
	uint8_t m_apiID;
	uint16_t m_version;
	uint16_t m_rangeMin;
	uint16_t m_rangeMax;

public:
	RangeSubAPI(uint8_t apiID, uint16_t version, uint16_t rangeMin,
	            uint16_t rangeMax)
	        : m_apiID(apiID),
	          m_version(version),
	          m_rangeMin(rangeMin),
	          m_rangeMax(rangeMax)
	{}

	uint8_t GetApiID() const override
	{
		return m_apiID;
	}
	uint16_t GetVersion() const override
	{
		return m_version;
	}
	bool HandlesCommand(uint16_t cmd) const override
	{
		return cmd >= m_rangeMin && cmd <= m_rangeMax;
	}
	bool GetCommandRange(uint16_t& min, uint16_t& max) const override
	{
		min = m_rangeMin;
		max = m_rangeMax;
		return true;
	}
};

class MappedSubAPI : public GeosHostSubAPI {
protected:
	uint8_t m_apiID;
	uint16_t m_version;
	const uint16_t* m_commands;
	int m_commandCount;

public:
	MappedSubAPI(uint8_t apiID, uint16_t version, const uint16_t* commands,
	             int count)
	        : m_apiID(apiID),
	          m_version(version),
	          m_commands(commands),
	          m_commandCount(count)
	{}

	uint8_t GetApiID() const override
	{
		return m_apiID;
	}
	uint16_t GetVersion() const override
	{
		return m_version;
	}
	bool HandlesCommand(uint16_t cmd) const override
	{
		for (int i = 0; i < m_commandCount; i++) {
			if (m_commands[i] == cmd) {
				return true;
			}
		}
		return false;
	}
	int GetCommandList(const uint16_t** outList) const override
	{
		*outList = m_commands;
		return m_commandCount;
	}
};

// Concrete sub-API declarations

static const uint16_t HostCommands[] = {HIF_SET_EVENT_INTERRUPT, HIF_GET_EVENT};

class HostSubAPI : public MappedSubAPI {
public:
	HostSubAPI()
	        : MappedSubAPI(HIF_API_HOST, 1, HostCommands,
	                       sizeof(HostCommands) / sizeof(uint16_t))
	{}
	void HandleCommand(uint16_t cmd) override;
};

static const uint16_t VideoCommands[] = {HIF_SET_VIDEO_PARAMS, HIF_GET_VIDEO_PARAMS};

class VideoSubAPI : public MappedSubAPI {
public:
	VideoSubAPI()
	        : MappedSubAPI(HIF_API_VIDEO, 1, VideoCommands,
	                       sizeof(VideoCommands) / sizeof(uint16_t))
	{}
	void HandleCommand(uint16_t cmd) override;
};

class NetworkSubAPI : public RangeSubAPI {
public:
	NetworkSubAPI()
	        : RangeSubAPI(HIF_API_SOCKET, 2, HIF_NETWORKING_BASE, HIF_NETWORKING_END)
	{}
	void HandleCommand(uint16_t cmd) override;
};

class SSLSubAPI : public RangeSubAPI {
public:
	SSLSubAPI() : RangeSubAPI(HIF_API_SSL, 1, HIF_SSL_BASE, HIF_SSL_END) {}
	void HandleCommand(uint16_t cmd) override;
};

// ============================================================
// Sub-API dispatcher
// ============================================================

#define MAX_SUB_APIS 8

static GeosHostSubAPI* G_subAPIs[MAX_SUB_APIS];
static int G_subAPICount = 0;

static bool CheckOverlap(const GeosHostSubAPI* a, const GeosHostSubAPI* b)
{
	// Check a's mapped commands against b
	const uint16_t* cmds;
	int count = a->GetCommandList(&cmds);
	for (int i = 0; i < count; i++) {
		if (b->HandlesCommand(cmds[i])) {
			return true;
		}
	}

	// Check b's mapped commands against a
	count = b->GetCommandList(&cmds);
	for (int i = 0; i < count; i++) {
		if (a->HandlesCommand(cmds[i])) {
			return true;
		}
	}

	// Range vs range intersection
	uint16_t aMin, aMax, bMin, bMax;
	if (a->GetCommandRange(aMin, aMax) && b->GetCommandRange(bMin, bMax)) {
		return aMin <= bMax && bMin <= aMax;
	}

	return false;
}

static bool RegisterSubAPI(GeosHostSubAPI* api)
{
	if (G_subAPICount >= MAX_SUB_APIS) {
		LOG_WARNING("GEOSHOST: Sub-API table full");
		return false;
	}

	for (int i = 0; i < G_subAPICount; i++) {
		if (CheckOverlap(api, G_subAPIs[i])) {
			LOG_WARNING("GEOSHOST: Sub-API %d command overlap with sub-API %d",
			            api->GetApiID(),
			            G_subAPIs[i]->GetApiID());
			return false;
		}
	}

	G_subAPIs[G_subAPICount++] = api;
	LOG_INFO("GEOSHOST: Registered sub-API %d (version %d)",
	         api->GetApiID(),
	         api->GetVersion());
	return true;
}

static uint16_t GetSubAPIVersion(uint8_t apiID)
{
	for (int i = 0; i < G_subAPICount; i++) {
		if (G_subAPIs[i]->GetApiID() == apiID) {
			return G_subAPIs[i]->GetVersion();
		}
	}
	return 0;
}

static bool DispatchToSubAPI(uint16_t cmd)
{
	for (int i = 0; i < G_subAPICount; i++) {
		if (G_subAPIs[i]->HandlesCommand(cmd)) {
			G_subAPIs[i]->HandleCommand(cmd);
			return true;
		}
	}
	return false;
}

static void PollSockets()
{

	for (int i = 1; i < MaxSockets; i++) {
		if (NetSockets[i].used && NetSockets[i].open &&
		    !NetSockets[i].ssl && !NetSockets[i].receiveDone &&
		    !NetSockets[i].done && (NetSockets[i].recvBufUsed <= 0)) {
			if (NetSockets[i].recvBuf == NULL) {
				GH_DBG("Socket %d: allocating recv buffer", i);
				NetSockets[i].recvBuf = new char[8192];
			}

			int result = NET_ReadFromStreamSocket(NetSockets[i].stream,
			                                      NetSockets[i].recvBuf,
			                                      8192);
			if (result < 0) {
				LOG_WARNING("GEOSHOST: Socket %d recv error", i);
				NetSockets[i].receiveDone   = true;
				NetSockets[i].sslInitialEnd = true;
				GeosHost_NotifySocketChange();
			} else if (result > 0) {
				NetSockets[i].recvBufUsed = result;
				GH_DBG("Socket %d: received %d bytes", i, result);
				GeosHost_NotifySocketChange();
			}
		}
	}
}

static void GeosHost_TickHandler(void)
{

	// Check for AsyncOp completion
	AsyncOp* nextOp = G_opRecords;
	while (nextOp) {

		nextOp->PollStatus();
		nextOp->HandleCompletion();
		nextOp = nextOp->GetNext();
	}
	if (G_opRecords) {
		G_opRecords = G_opRecords->Cleanup();
	}
	if (G_protectedOpMode == cpu.pmode) {
		PollSockets();
	}

	// if in the matching operation mode: real mode or protected mode
	if (G_eventInterrupt && (G_protectedOpMode == cpu.pmode)) {
		// if event interrupt is requested
		if (G_recheckEventInterrupt && (reg_flags & FLAG_IF)) {

			static int intCounter = 0;

			SDL_mutexP(G_eventQueueMutex);
			G_recheckEventInterrupt = false;
			SDL_mutexV(G_eventQueueMutex);
			// issue software interrupt
			int thisIntCount = intCounter++;

			GH_DBG("Trigger event interrupt (#%d)", thisIntCount);

			CPU_SW_Interrupt(G_eventInterrupt, reg_eip);
			GH_DBG("Event interrupt done (#%d)", thisIntCount);
		}
	}
}

void GeosHost_SendEvent(uint16_t* eventRecord)
{
	// Create Event Object
	EventRecord* newRecord = new EventRecord(eventRecord, NULL);

	// Add Event to list
	SDL_mutexP(G_eventQueueMutex);
	if (G_eventRecords == NULL) {
		G_recheckEventInterrupt = true;
	}
	newRecord->SetNext(G_eventRecords);
	G_eventRecords = newRecord;
	SDL_mutexV(G_eventQueueMutex);
}

uint16_t AsyncSocketSend::Init(uint16_t* cmdRec)
{

	m_socketHandle = cmdRec[1];

	GH_DBG("SocketSend::Init: size=%d seg:off=%x:%x socket=%d",
	       cmdRec[4],
	       cmdRec[2],
	       cmdRec[3],
	       m_socketHandle);

	if (m_socketHandle < 0 || m_socketHandle >= MaxSockets) {
		LOG_WARNING("GEOSHOST: SocketSend: invalid handle %d", m_socketHandle);
		return HIF_FAILED;
	} else {
		SocketState& sock = NetSockets[m_socketHandle];

		int size = cmdRec[4];
		GH_DBG("SocketSend: data size=%d", size);

		char* buffer = new char[size + 1];

		ReadDosMemory(G_commandBuffer[2], G_commandBuffer[3], buffer, size);
		buffer[size] = 0;

		bool result = NET_WriteToStreamSocket(sock.stream, buffer, size);
		delete[] buffer;
		if (!result) {
			LOG_WARNING("GEOSHOST: SocketSend: write failed on socket %d",
			            m_socketHandle);
			return HIF_FAILED;
		}
	}
	return HIF_OK;
}

uint16_t AsyncSocketSend::RunAsync()
{
	return 0;
}

uint16_t AsyncSocketSend::PollStatus()
{
	SocketState& sock = NetSockets[m_socketHandle];
	if (sock.stream) {
		int pendingCount = NET_GetStreamSocketPendingWrites(sock.stream);
		if (pendingCount == 0) {

			// successfully sent
			m_Result[0] = HIF_OK;
		} else {
			// ended with error
			m_Result[0] = HIF_FAILED;
		}
	}
	return 0;
}

uint16_t AsyncSocketResolveAddr::Init(uint16_t* cmdRec)
{
	// si:bx	= host name address
	// cx = address name len
	GH_DBG("ResolveAddr::Init: seg=%x off=%x", cmdRec[1], cmdRec[2]);

	MEM_StrCopy(ResolveAddress(cmdRec[1], cmdRec[2]), m_hostname, cmdRec[3]);
	m_hostname[cmdRec[3]] = 0;

	m_RunOwnThread = false;
	m_Addr         = NET_ResolveHostname(m_hostname);
	if (m_Addr == NULL) {
		LOG_WARNING("GEOSHOST: Failed to resolve hostname: %s", m_hostname);
		return HIF_FAILED;
	}

	return AsyncOp::Init(cmdRec);
}

uint16_t AsyncSocketConnect::Init(uint16_t* cmdRec)
{

	m_socketHandle = cmdRec[4];

	m_RunOwnThread = false;
	char hostname[20];
	sprintf(hostname,
	        "%d.%d.%d.%d",
	        cmdRec[2] & 0xFF,
	        (cmdRec[2] >> 8) & 0xFF,
	        cmdRec[1] & 0xFF,
	        (cmdRec[1] >> 8) & 0xFF);
	LOG_INFO("GEOSHOST: Connect to %s:%d (socket %d)", hostname, m_Port, m_socketHandle);
	m_Port = cmdRec[3] /* ((cmdRec[3] & 0xFF) << 8) |
	         ((cmdRec[3] >> 8) & 0xFF)*/
	        ;
	m_Addr = NET_ResolveHostname(hostname);
	if (m_Addr == NULL) {
		LOG_WARNING("GEOSHOST: Failed to resolve: %s", hostname);
		return HIF_FAILED;
	}
	m_State = RESOLVING;

	return AsyncOp::Init(cmdRec);
}

uint16_t AsyncSocketConnect::RunAsync()
{

	return 0;
}

uint16_t AsyncSocketConnect::PollStatus()
{

	switch (m_State) {
	case RESOLVING:
		if (m_Addr) {
			NET_Status status = NET_GetAddressStatus(m_Addr);
			if (status != NET_WAITING) {
				if (status == NET_SUCCESS) {
					m_State = CONNECTING;
					NetSockets[m_socketHandle].stream =
					        NET_CreateClient(m_Addr, m_Port);
				} else {
					LOG_WARNING("GEOSHOST: Address resolution failed for socket %d",
					            m_socketHandle);
					m_State     = DONE;
					m_Result[0] = HIF_FAILED;
				}
				NET_UnrefAddress(m_Addr);
			}
		}
		break;
	case CONNECTING:
		if (NetSockets[m_socketHandle].stream) {
			NET_Status status = NET_GetConnectionStatus(
			        NetSockets[m_socketHandle].stream);
			if (status != NET_WAITING) {
				if (status == NET_SUCCESS) {
					m_State = CONNECTED;
					LOG_INFO("GEOSHOST: Socket %d connected",
					         m_socketHandle);
					m_Result[0] = HIF_OK;
					m_State     = DONE;
				} else {
					LOG_WARNING("GEOSHOST: Socket %d connection failed",
					            m_socketHandle);
					m_Result[0] = HIF_FAILED;
					m_State     = DONE;
				}
			}
		}
		break;
	default: break;
	}
	return 0;
}

static int InitOpThread(void* paramsPtr)
{
	((AsyncOp*)paramsPtr)->RunAsync();
	return 0;
}

uint16_t AsyncOp::Init(uint16_t* cmdRec)
{
	// allocate ID
	m_ID = G_nextAsyncID;
	G_nextAsyncID++;

	uint16_t checkSlot = 0;
	while (checkSlot < MAX_ASYNC_OP_SLOTS) {

		AsyncOp* nextOp = G_opRecords;
		while (nextOp) {

			if (nextOp->GetID() == checkSlot) {
				break;
			}
			nextOp = nextOp->GetNext();
		}

		if (!nextOp) {
			// not found, so we are free to use the slot
			break;
		}

		checkSlot++;
	}

	if (checkSlot == MAX_ASYNC_OP_SLOTS) {
		LOG_WARNING("GEOSHOST: All %d async op slots full",
		            MAX_ASYNC_OP_SLOTS);
		return HIF_FAILED;
	}

	m_ID = checkSlot;

	// Simply create a thread
	if (m_RunOwnThread) {
		m_thread = SDL_CreateThread(InitOpThread, "InitOpThread", (void*)this);

		if (NULL == m_thread) {
			LOG_WARNING("GEOSHOST: SDL_CreateThread failed: %s",
			            SDL_GetError());
			return HIF_NO_MEMORY;
		}
		SDL_DetachThread(m_thread);
	}

	return HIF_PENDING | (m_ID << 8);
}

void AsyncOp::HandleCompletion()
{
	if (!m_EventSent && m_Result[0] != HIF_PENDING) {

		m_Result[0] |= m_ID << 8;
		GeosHost_SendEvent(m_Result);

		m_EventSent = true;
	}
}

AsyncOp* AsyncOp::Cleanup()
{
	// Remove completed ops from the tail of the list
	AsyncOp* prev = this;
	AsyncOp* curr = this->GetNext();
	while (curr) {
		if (curr->m_EventSent) {
			prev->m_Next = curr->GetNext();
			delete curr;
			curr = prev->m_Next;
		} else {
			prev = curr;
			curr = curr->GetNext();
		}
	}

	// If head itself is complete, remove it
	if (this->m_EventSent) {
		AsyncOp* newHead = this->GetNext();
		delete this;
		return newHead;
	}
	return this;
}

uint16_t AsyncSocketResolveAddr::RunAsync()
{
	m_Result[0] = HIF_FAILED;

	return 0;
}

uint16_t AsyncSocketResolveAddr::PollStatus()
{

	NET_Status status = NET_GetAddressStatus(m_Addr);
	if (m_Addr && (status != NET_WAITING)) {
		if (status == NET_SUCCESS) {
			m_Result[0]    = HIF_OK;
			long addr_host = NET_GetIP4Address(m_Addr);
			LOG_INFO("GEOSHOST: Address resolved: %x", addr_host);
			m_Result[1] = ((addr_host >> 16) & 0xFF) |
			              ((addr_host >> 16) & 0xFF00);
			m_Result[2] = (addr_host & 0xFF) | (addr_host & 0xFF00);
		} else {
			m_Result[0] = HIF_FAILED;
		}
		NET_UnrefAddress(m_Addr);
		m_Addr = NULL;
	}
	return 0;
}

static uint16_t read_baseboxid(io_port_t, io_width_t)
{
	uint16_t result = 0;
	if (G_responseOffset > 0) {
		G_responseOffset--;
		result = G_responseBuffer[G_responseOffset];
	}
	G_commandOffset = 0;
	return result;
}

void GeosHost_NotifyVideoChange()
{
	// send event to GEOS client
	static uint16_t eventRecord[6];
	eventRecord[0] = HIF_EVENT_NOTIFICATION;
	eventRecord[1] = HIF_NOTIFY_DISPLAY_SIZE_CHANGE;

	GeosHost_SendEvent(eventRecord);
}

void GeosHost_NotifySocketChange()
{
	// send event to GEOS client
	static uint16_t eventRecord[6];
	eventRecord[0] = HIF_EVENT_NOTIFICATION;
	eventRecord[1] = HIF_NOTIFY_SOCKET_STATE_CHANGE;

	GeosHost_SendEvent(eventRecord);
}

#define MAX_HANDLES 20

static void* handles[MAX_HANDLES];
static uint16_t associatedSocket[MAX_HANDLES];

static int AllocHandle(void* ptr)
{

	int handle = 0;
	while (handle < MAX_HANDLES) {

		if (handles[handle] == NULL) {

			handles[handle] = ptr;
			return handle + 1;
		}
		handle++;
	}

	LOG_WARNING("GEOSHOST: Handle table full (%d slots)", MAX_HANDLES);
	return 0;
}

uint16_t AsyncSSLConnect::RunAsync()
{
	return 0;
}

int validate_certificate(struct TLSContext* context,
                         struct TLSCertificate** certificate_chain, int len)
{
	int i;
	int err;
	if (certificate_chain) {
		for (i = 0; i < len; i++) {
			struct TLSCertificate* certificate = certificate_chain[i];
			// check validity date
			err = tls_certificate_is_valid(certificate);
			if (err) {
				return err;
			}
			// check certificate in certificate->bytes of length
			// certificate->len the certificate is in ASN.1 DER format
		}
	}
	// check if chain is valid
	err = tls_certificate_chain_is_valid(certificate_chain, len);
	if (err) {
		return err;
	}

	const char* sni = tls_sni(context);
	if ((len > 0) && (sni)) {
		err = tls_certificate_valid_subject(certificate_chain[0], sni);
		if (err) {
			return err;
		}
	}

	// err = tls_certificate_chain_is_valid_root(context, certificate_chain,
	// len); if (err) { 	return err;
	// }

	GH_DBG("Certificate validation OK");

	// return certificate_expired;
	// return certificate_revoked;
	// return certificate_unknown;
	return no_error;
}

uint16_t AsyncSSLConnect::Init(uint16_t* cmdRec)
{

	m_RunOwnThread = false;

	int handle = G_commandBuffer[HIF_SLOT_SI];
	GH_DBG("SSLConnect::Init handle=%x", handle);

	m_ctx = reinterpret_cast<struct TLSContext*>(handles[handle - 1]);
	m_socketHandle = associatedSocket[handle - 1];

	int res = tls_client_connect(m_ctx);
	if (res < 0) {
		LOG_WARNING("GEOSHOST: tls_client_connect failed: %d", res);
		return HIF_FAILED;
	}

	unsigned int out_buffer_len;
	const unsigned char* out_buffer = tls_get_write_buffer(m_ctx, &out_buffer_len);

	bool result = NET_WriteToStreamSocket(NetSockets[m_socketHandle].stream,
	                                      out_buffer,
	                                      out_buffer_len);
	if (!result) {
		LOG_WARNING("GEOSHOST: SSLConnect initial write failed on socket %d",
		            m_socketHandle);
		return HIF_FAILED;
	}

	m_State = SENDING;

	return AsyncOp::Init(cmdRec);
}

uint16_t AsyncSSLConnect::PollStatus()
{
	SocketState& sock = NetSockets[m_socketHandle];
	switch (m_State) {

	case SENDING:
		if (sock.stream) {
			int pendingCount = NET_GetStreamSocketPendingWrites(
			        sock.stream);
			if (pendingCount == 0) {

				tls_buffer_clear(m_ctx);

				if (tls_established(m_ctx) == 1) {

					LOG_INFO("GEOSHOST: TLS handshake complete (socket %d)",
					         m_socketHandle);
					m_Result[0]           = HIF_OK;
					m_Result[HIF_SLOT_DX] = 1;
					m_State               = DONE;
				} else {
					m_State = RECEIVING;
				}

			} else if (pendingCount < 0) {
				LOG_WARNING("GEOSHOST: TLS handshake write error (socket %d)",
				            m_socketHandle);
				m_Result[0] = HIF_FAILED;
				m_State     = DONE;
			}
		}
		break;
	case RECEIVING: {
		int result = NET_ReadFromStreamSocket(sock.stream,
		                                      m_Buffer,
		                                      sizeof(m_Buffer));
		if (result < 0) {
			LOG_WARNING("GEOSHOST: TLS handshake recv error (socket %d)",
			            m_socketHandle);
			sock.receiveDone   = true;
			sock.sslInitialEnd = true;
			GeosHost_NotifySocketChange();

			m_Result[0] = HIF_FAILED;
			m_State     = DONE;

		} else if (result > 0) {

			tls_consume_stream(m_ctx, m_Buffer, result, validate_certificate);

			if (tls_established(m_ctx) == 1) {

				LOG_INFO("GEOSHOST: TLS handshake complete (socket %d)",
				         m_socketHandle);
				m_Result[0]           = HIF_OK;
				m_Result[HIF_SLOT_DX] = 1;
				m_State               = DONE;
			} else {
				unsigned int out_buffer_len;
				const unsigned char* out_buffer =
				        tls_get_write_buffer(m_ctx, &out_buffer_len);

				if (out_buffer) {
					m_State     = SENDING;
					bool result = NET_WriteToStreamSocket(
					        NetSockets[m_socketHandle].stream,
					        out_buffer,
					        out_buffer_len);
					if (!result) {
						LOG_WARNING("GEOSHOST: TLS handshake write failed (socket %d)",
						            m_socketHandle);
						return HIF_FAILED;
					}
				}
			}
		}
	} break;
	}

	return 0;
}

uint16_t AsyncSSLWrite::Init(uint16_t* cmdRec)
{

	m_RunOwnThread = false;

	int handle = G_commandBuffer[HIF_SLOT_SI];
	GH_DBG("SSLWrite::Init handle=%x", handle);

	m_ctx = reinterpret_cast<struct TLSContext*>(handles[handle - 1]);
	m_socketHandle = associatedSocket[handle - 1];

	int size = cmdRec[5];
	GH_DBG("SSLWrite: data size=%d", size);

	char* buffer = new char[size + 1];

	ReadDosMemory(G_commandBuffer[4], G_commandBuffer[3], buffer, size);
	buffer[size] = 0;

	m_written = tls_write(m_ctx, (const unsigned char*)buffer, (unsigned int)size);
	if (m_written < 0) {
		LOG_WARNING("GEOSHOST: tls_write failed: %d", m_written);
		return HIF_FAILED;
	}

	unsigned int out_buffer_len;
	const unsigned char* out_buffer = tls_get_write_buffer(m_ctx, &out_buffer_len);

	bool result = NET_WriteToStreamSocket(NetSockets[m_socketHandle].stream,
	                                      out_buffer,
	                                      out_buffer_len);
	if (!result) {
		LOG_WARNING("GEOSHOST: SSLWrite stream write failed on socket %d",
		            m_socketHandle);
		return HIF_FAILED;
	}

	return AsyncOp::Init(cmdRec);
}

uint16_t AsyncSSLWrite::RunAsync()
{
	return 0;
}

uint16_t AsyncSSLWrite::PollStatus()
{

	SocketState& sock = NetSockets[m_socketHandle];
	if (sock.stream) {
		int pendingCount = NET_GetStreamSocketPendingWrites(sock.stream);
		if (pendingCount == 0) {

			// successfully sent
			tls_buffer_clear(m_ctx);

			m_Result[0]           = HIF_OK;
			m_Result[HIF_SLOT_DX] = m_written;

		} else if (pendingCount < 0) {
			LOG_WARNING("GEOSHOST: SSLWrite pending error on socket %d",
			            m_socketHandle);
			m_Result[0] = HIF_FAILED;
		}
	}

	return 0;
}

uint16_t AsyncSSLRead::Init(uint16_t* cmdRec)
{

	m_RunOwnThread = false;

	int handle = G_commandBuffer[HIF_SLOT_SI];
	GH_DBG("SSLRead::Init handle=%x", handle);

	m_ctx = reinterpret_cast<struct TLSContext*>(handles[handle - 1]);
	m_socketHandle = associatedSocket[handle - 1];
	GH_DBG("SSLRead::Init socket=%x", m_socketHandle);

	m_BufferSegment = G_commandBuffer[4];
	m_BufferOffset  = G_commandBuffer[3];
	m_BufferSize    = cmdRec[5];
	GH_DBG("SSLRead::Init bufferSize=%d", m_BufferSize);

	return AsyncOp::Init(cmdRec);
}

uint16_t AsyncSSLRead::RunAsync()
{
	return 0;
}

bool IsSegmentAccessible(uint16_t selector, bool isWrite = false)
{
	// Null selector
	if ((selector & 0xFFFC) == 0) {
		return false;
	}

	// Real Mode oder VM86: keine Deskriptor-Pr�fung n�tig
	if (!cpu.pmode || (reg_flags & FLAG_VM)) {
		return true;
	}

	// LDT-Selektor: pr�fen ob LDT �berhaupt geladen ist
	if (selector & 4) {
		if ((cpu.gdt.SLDT() & 0xFFFC) == 0) {
			return false;
		}
	}

	Descriptor desc;
	if (!cpu.gdt.GetDescriptor(selector, desc)) {
		return false;
	}

	// Present-Bit pr�fen
	if (!desc.saved.seg.p) {
		return false;
	}

	// DPL vs CPL/RPL
	uint8_t rpl = selector & 3;
	uint8_t eff = (cpu.cpl > rpl) ? cpu.cpl : rpl;
	if (desc.DPL() < eff) {
		return false;
	}

	// Typ-Pr�fung
	uint8_t type = desc.Type();
	if (isWrite) {
		// Muss beschreibbares Datensegment sein
		if ((type & 0x8) || !(type & 0x2)) {
			return false;
		}
	} else {
		// Muss Code- oder Datensegment sein (kein System-Deskriptor)
		if (!(type & 0x10)) {
			return false;
		}
	}

	return true;
}

uint16_t AsyncSSLRead::PollStatus()
{
	if (G_protectedOpMode) {
		if (!cpu.pmode) {

			return 0;
		}
		if (!IsSegmentAccessible(m_BufferSegment, true)) {
			return 0;
		}
	} else if (cpu.pmode) {
		return 0;
	}

	int read_size = tls_read(m_ctx,
	                         m_Buffer,
	                         m_BufferSize > sizeof(m_Buffer) ? sizeof(m_Buffer)
	                                                         : m_BufferSize);
	if (read_size > 0) {

		// done some
		m_Result[0]           = HIF_OK;
		m_Result[HIF_SLOT_DX] = read_size;

		WriteDosMemory(m_BufferSegment, m_BufferOffset, m_Buffer, read_size);

	} else {
		SocketState& sock = NetSockets[m_socketHandle];
		if (sock.stream) {

			int result = NET_ReadFromStreamSocket(sock.stream,
			                                      m_Buffer,
			                                      sizeof(m_Buffer));
			if (result < 0) {
				LOG_WARNING("GEOSHOST: SSLRead recv error on socket %d",
				            m_socketHandle);
				sock.receiveDone   = true;
				sock.sslInitialEnd = true;
				GeosHost_NotifySocketChange();

				m_Result[0] = HIF_FAILED;

			} else if (result > 0) {

				// some data to consume
				tls_consume_stream(m_ctx,
				                   m_Buffer,
				                   result,
				                   validate_certificate);
			}
		}
	}

	return 0;
}

template <typename T>
static void DispatchAsyncOp()
{
	AsyncOp* newOp = new T(G_opRecords);
	if (newOp) {
		G_responseBuffer[0] = newOp->Init(G_commandBuffer);
		if ((G_responseBuffer[0] & 0xFF) != HIF_PENDING) {
			delete newOp;
		} else {
			G_opRecords = newOp;
		}
	} else {
		LOG_WARNING("GEOSHOST: Failed to allocate async op");
		G_responseBuffer[0] = HIF_NO_MEMORY;
	}
	G_responseOffset = 6;
}

// ============================================================
// Sub-API HandleCommand implementations
// ============================================================

void HostSubAPI::HandleCommand(uint16_t cmd)
{
	switch (cmd) {
	case HIF_SET_EVENT_INTERRUPT:
		G_protectedOpMode = cpu.pmode;
		G_eventInterrupt  = 0xA0 /* G_commandBuffer[1] & 0xFF*/;
		break;

	case HIF_GET_EVENT:
		SDL_mutexP(G_eventQueueMutex);
		if (G_eventRecords == NULL) {
			G_responseBuffer[0] = HIF_NOT_FOUND;
		} else {
			EventRecord* thisRecord = G_eventRecords;
			thisRecord->GetRecordData(G_responseBuffer);
			G_eventRecords = thisRecord->GetNext();
			delete thisRecord;
		}
		G_responseOffset = 6;
		SDL_mutexV(G_eventQueueMutex);
		break;
	}
}

void VideoSubAPI::HandleCommand(uint16_t cmd)
{
	switch (cmd) {
	case HIF_SET_VIDEO_PARAMS: {
		MOUSEDOS_BeforeNewVideoMode();
		uint16_t newWidth  = G_commandBuffer[1];
		uint16_t newHeight = G_commandBuffer[2];
		VESA_SetBaseboxMode(newWidth, newHeight);
		LOG_INFO("GEOSHOST: Set video mode %dx%d", newWidth, newHeight);
		MOUSEDOS_AfterNewVideoMode(false);

		G_responseBuffer[0] = HIF_OK;
		G_responseBuffer[2] = 0x89A;
		G_responseOffset    = 6;
		break;
	}

	case HIF_GET_VIDEO_PARAMS: {
		float ddpi, hdpi, vdpi;

		int displayIndex = SDL_GetWindowDisplayIndex(GFX_GetWindow());

		int width, height;
		SDL_GL_GetDrawableSize(GFX_GetWindow(), &width, &height);

		G_responseBuffer[0] = HIF_OK;
		G_responseBuffer[1] = width;
		G_responseBuffer[2] = height;

		if (SDL_GetDisplayDPI(displayIndex, &ddpi, &hdpi, &vdpi) == 0) {
			G_responseBuffer[3] = hdpi;
			G_responseBuffer[4] = vdpi;
		} else {
			G_responseBuffer[3] = 0;
			G_responseBuffer[4] = 0;
		}
		GH_DBG("Display resolution: %dx%d dpi=%dx%d",
		       width,
		       height,
		       G_responseBuffer[3],
		       G_responseBuffer[4]);
		G_responseBuffer[5] = 0;
		G_responseOffset    = 6;
		break;
	}
	}
}

void NetworkSubAPI::HandleCommand(uint16_t cmd)
{
	switch (cmd) {
	case HIF_NC_RESOLVE_ADDR:
		DispatchAsyncOp<AsyncSocketResolveAddr>();
		break;

	case HIF_NC_ALLOC_CONNECTION: {
		int socketHandle = -1;

		int i = G_lookupNext;
		do {
			if (!NetSockets[i].used) {
				socketHandle = i;
				break;
			}
			i++;
			if (i == MaxSockets) {
				i = 1;
			}
		} while (i != G_lookupNext);

		if (socketHandle != -1) {
			G_lookupNext = i + 1;
			if (G_lookupNext >= MaxSockets) {
				G_lookupNext = 1;
			}
		}

		if (socketHandle < 0) {
			LOG_WARNING("GEOSHOST: No free sockets");
			G_responseBuffer[0] = HIF_TABLE_FULL;
			return;
		}

		LOG_INFO("GEOSHOST: Allocated socket %d", socketHandle);
		SocketState& sock = NetSockets[socketHandle];

		sock.used          = true;
		sock.done          = false;
		sock.open          = false;
		sock.blocking      = false;
		sock.ssl           = false;
		sock.sslInitialEnd = false;
		sock.receiveDone   = false;
		sock.sendDone      = false;

		G_responseBuffer[0] = HIF_OK;
		G_responseBuffer[1] = (uint16_t)socketHandle;
		G_responseOffset    = 6;
		break;
	}

	case HIF_NC_CONNECT_REQUEST:
		DispatchAsyncOp<AsyncSocketConnect>();
		break;

	case HIF_NC_SEND_DATA: DispatchAsyncOp<AsyncSocketSend>(); break;

	case HIF_NC_RECV_NEXT_CLOSE: {
		G_responseBuffer[0] = HIF_OK;
		for (int i = 0; i < MaxSockets; i++) {
			if (NetSockets[i].used && !NetSockets[i].done) {
				if (NetSockets[i].receiveDone) {
					NetSockets[i].done  = true;
					G_responseBuffer[1] = i;
					G_responseBuffer[2] = NetSockets[i].sendDone
					                            ? 0xFFFF
					                            : 0;
					if (i > 0) {
						GH_DBG("RecvNextClosed: socket=%d sendDone=%x",
						       i,
						       G_responseBuffer[2]);
					}
					G_responseOffset = 6;

					if (NetSockets[i].sendDone) {
						NetSockets[i].used = false;
					}
					return;
				}
			}
		}
		G_responseBuffer[1] = 0;
		G_responseOffset    = 6;
		break;
	}

	case HIF_NC_NEXT_RECV_SIZE: {
		G_responseBuffer[0] = HIF_OK;
		G_responseBuffer[1] = 0;

		for (int i = 0; i < MaxSockets; i++) {
			if (NetSockets[i].used && !NetSockets[i].done) {
				if (NetSockets[i].recvBufUsed > 0) {
					G_responseBuffer[1] = NetSockets[i].recvBufUsed;
					G_responseBuffer[2] = i;
					break;
				}
			}
		}
		if (G_responseBuffer[1] > 0) {
			GH_DBG("NextRecvSize: %d bytes on socket %d",
			       G_responseBuffer[1],
			       G_responseBuffer[2]);
		}
		G_responseOffset = 6;
		break;
	}

	case HIF_NC_RECV_NEXT: {
		for (int i = 0; i < MaxSockets; i++) {
			if (NetSockets[i].used && !NetSockets[i].done) {
				GH_DBG("RecvNext: socket=%u bufUsed=%d expected=%d",
				       i,
				       NetSockets[i].recvBufUsed,
				       G_commandBuffer[1]);

				if (NetSockets[i].recvBufUsed == G_commandBuffer[1]) {
					int size = G_commandBuffer[1];

					GH_DBG("RecvNext: copying %d bytes to %x:%x",
					       size,
					       G_commandBuffer[2],
					       G_commandBuffer[3]);
					WriteDosMemory(G_commandBuffer[2],
					               G_commandBuffer[3],
					               NetSockets[i].recvBuf,
					               size);

					NetSockets[i].recvBufUsed = 0;
					G_responseBuffer[1]       = i;
					break;
				}
			}
			G_responseOffset = 6;
		}
		break;
	}

	case HIF_NC_DISCONNECT: {
		LOG_INFO("GEOSHOST: Disconnect socket=%d full=%d",
		         G_commandBuffer[1],
		         G_commandBuffer[2]);
		SocketState& sock = NetSockets[G_commandBuffer[1]];

		if (sock.used) {
			sock.sendDone = true;
			if (sock.stream) {
				NET_DestroyStreamSocket(sock.stream);
				sock.stream      = NULL;
				sock.receiveDone = true;
				sock.done        = false;
			}

			if (G_commandBuffer[2]) {
				sock.used = false;
				sock.done = true;
			}
		}
		G_responseBuffer[0] = HIF_OK;
		G_responseOffset    = 6;
		break;
	}

	case HIF_NC_CONNECTED: {
		LOG_INFO("GEOSHOST: Socket %d marked connected", G_commandBuffer[1]);
		SocketState& sock = NetSockets[G_commandBuffer[1]];

		if (sock.used) {
			sock.open = true;
		}
		G_responseBuffer[0] = HIF_OK;
		G_responseOffset    = 6;
		break;
	}
	}
}

void SSLSubAPI::HandleCommand(uint16_t cmd)
{
	switch (cmd) {
	case HIF_SSL_CTX_NEW: {
		int method = 0;
		int handle = AllocHandle(tls_create_context(method, TLS_V12));

		GH_DBG("SSL_CTX_NEW: handle=%x", handle);
		G_responseBuffer[HIF_SLOT_AX] = HIF_OK;
		G_responseBuffer[HIF_SLOT_DX] = handle & 0xFFFF;
		G_responseOffset              = 6;
		break;
	}

	case HIF_SSL_NEW: {
		int handle = G_commandBuffer[HIF_SLOT_SI];

		struct TLSContext* context = reinterpret_cast<struct TLSContext*>(
		        handles[handle - 1]);

		GH_DBG("SSL_NEW: ctx_handle=%d context=%p", handle, context);

		int method = 0;
		struct TLSContext* context2 = tls_create_context(method, TLS_V12);
		int newHandle = AllocHandle(context2);

		GH_DBG("SSL_NEW: ssl_handle=%d", newHandle);
		G_responseBuffer[HIF_SLOT_AX] = HIF_OK;
		G_responseBuffer[HIF_SLOT_DX] = newHandle & 0xFFFF;
		G_responseOffset              = 6;
		break;
	}

	case HIF_SSL_SET_SSL_METHOD:
		G_responseBuffer[HIF_SLOT_AX] = HIF_FAILED;
		G_responseBuffer[HIF_SLOT_DX] = 0;
		G_responseOffset              = 6;
		break;

	case HIF_SSL_SET_FD: {
		int handle = G_commandBuffer[HIF_SLOT_SI];
		int socket = G_commandBuffer[HIF_SLOT_DI];
		GH_DBG("SSL_SET_FD: handle=%x socket=%x", handle, socket);
		associatedSocket[handle - 1] = socket;

		NetSockets[socket].ssl = true;

		G_responseBuffer[HIF_SLOT_AX] = HIF_FAILED;
		G_responseBuffer[HIF_SLOT_DX] = 0;
		G_responseOffset              = 6;
		break;
	}

	case HIF_SSL_SET_TLSEXT_HOST_NAME: {
		char host[256];

		int handle = G_commandBuffer[HIF_SLOT_SI];

		struct TLSContext* ctx = reinterpret_cast<struct TLSContext*>(
		        handles[handle - 1]);

		// TODO check buffer size
		MEM_StrCopy(ResolveAddress(G_commandBuffer[HIF_SLOT_DX],
		                           G_commandBuffer[HIF_SLOT_CX]),
		            host,
		            reg_di);
		host[G_commandBuffer[HIF_SLOT_DI]] = 0;

		LOG_INFO("GEOSHOST: SNI hostname set: %s (handle=%x)", host, handle);
		tls_sni_set(ctx, host);

		G_responseBuffer[HIF_SLOT_AX] = HIF_OK;
		G_responseBuffer[HIF_SLOT_DX] = 0;
		break;
	}

	case HIF_SSL_CONNECT: DispatchAsyncOp<AsyncSSLConnect>(); break;

	case HIF_SSL_WRITE: DispatchAsyncOp<AsyncSSLWrite>(); break;

	case HIF_SSL_READ: DispatchAsyncOp<AsyncSSLRead>(); break;

	case HIF_SSL_CTX_FREE: {
		int handle = G_commandBuffer[HIF_SLOT_SI];
		tls_destroy_context((struct TLSContext*)handles[handle - 1]);
		handles[handle - 1]           = NULL;
		G_responseBuffer[HIF_SLOT_AX] = HIF_OK;
		break;
	}

	case HIF_SSL_FREE: {
		int handle = G_commandBuffer[HIF_SLOT_SI];
		tls_destroy_context((struct TLSContext*)handles[handle - 1]);
		handles[handle - 1]           = NULL;
		G_responseBuffer[HIF_SLOT_AX] = HIF_OK;
		break;
	}
	}
}

// ============================================================
// I/O port command handler
// ============================================================

static void write_baseboxcmd(io_port_t, io_val_t command, io_width_t)
{
	if (G_commandOffset < (sizeof(G_commandBuffer) / sizeof(uint16_t))) {
		G_commandBuffer[G_commandOffset] = (uint16_t)command;
		G_commandOffset++;

		if (G_commandOffset == (sizeof(G_commandBuffer) / sizeof(uint16_t))) {

			if (G_commandBuffer[0] == HIF_CHECK_API) {
				// Meta-command: query sub-API versions
				G_responseBuffer[0] = HIF_OK;
				G_responseBuffer[1] = (((uint16_t)G_baseboxID[1])
				                       << 8) |
				                      G_baseboxID[0];
				G_responseBuffer[2] = (((uint16_t)G_baseboxID[3])
				                       << 8) |
				                      G_baseboxID[2];
				G_responseBuffer[3] = (((uint16_t)G_baseboxID[5])
				                       << 8) |
				                      G_baseboxID[4];
				G_responseBuffer[4] = (((uint16_t)G_baseboxID[7])
				                       << 8) |
				                      G_baseboxID[6];
				G_responseBuffer[5] = GetSubAPIVersion(
				        G_commandBuffer[1]);
				LOG_INFO("GEOSHOST: API check: api=%d version=%d",
				         G_commandBuffer[1],
				         G_responseBuffer[5]);
				G_responseOffset = 6;
			} else if (!DispatchToSubAPI(G_commandBuffer[0])) {
				LOG_WARNING("GEOSHOST: Unhandled request code: %d",
				            G_commandBuffer[0]);
			}
		}
	}
}

void GEOSHOST_Init()
{

	NET_Init();

	IO_RegisterReadHandler(0x38FF, read_baseboxid, io_width_t::word);
	IO_RegisterWriteHandler(0x38FF, write_baseboxcmd, io_width_t::word);

	G_eventQueueMutex = SDL_CreateMutex();

	// Register sub-APIs
	static HostSubAPI hostAPI;
	static VideoSubAPI videoAPI;
	static NetworkSubAPI networkAPI;
	static SSLSubAPI sslAPI;

	RegisterSubAPI(&hostAPI);
	RegisterSubAPI(&videoAPI);
	RegisterSubAPI(&networkAPI);
	RegisterSubAPI(&sslAPI);

	TIMER_AddTickHandler(GeosHost_TickHandler);

	LOG_INFO("GEOSHOST: Initialized");
}

void GEOSHOST_Exit()
{
	LOG_INFO("GEOSHOST: Shutting down");
	NET_Quit();
}

#endif // C_GEOSHOST
