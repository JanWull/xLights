#include <wx/wx.h>
#include <wx/sstream.h>
#include <wx/config.h>
#include <wx/regex.h>

#ifdef __WXMSW__
#include <iphlpapi.h>
#endif

#include "ScanWork.h"
#include "../xLights/Parallel.h"
#include "xScannerMain.h"
#include "xScannerApp.h"
#include "../xSchedule/xSMSDaemon/Curl.h"
#include "../xLights/UtilFunctions.h"
#include "../xSchedule/wxJSON/jsonreader.h"
#include "../xLights/controllers/Falcon.h"
#include "../xLights/controllers/Pixlite16.h"
#include "../xLights/outputs/ControllerEthernet.h"
#include "../xLights/Discovery.h"
#include "../xLights/outputs/OutputManager.h"
#include "../xLights/outputs/ZCPPOutput.h"
#include "../xLights/outputs/DDPOutput.h"
#include "../xLights/outputs/ArtNetOutput.h"
#include "../xLights/controllers/FPP.h"
#include "../xLights/outputs/ControllerEthernet.h"
#include "MAC.h"

#include <log4cpp/Category.hh>

#define FAST_TIMEOUT 2
#define SLOW_TIMEOUT 5
#define WORKER_THREADS 15

WorkManager::WorkManager()
{
}

void WorkManager::Start()
{
	static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));

	if (_threadsPing.size() == 0) {
		if (_singleThreaded) {
			// we cant actually run single threaded so we run 1 ping, 1 main and 2 other (as computer is long running in other)
			_threadsPing.push_back(new ScanThread(*this, ThreadType::TTPING));
			_threadsOther.push_back(new ScanThread(*this, ThreadType::TTOTHER));
			_threadsOther.push_back(new ScanThread(*this, ThreadType::TTOTHER));
			_threadsMain.push_back(new ScanThread(*this, ThreadType::TTMAIN, new wxSocketClient(wxSOCKET_WAITALL | wxSOCKET_BLOCK)));
		}
		else {
			for (size_t i = 0; i < WORKER_THREADS; i++) {
				_threadsPing.push_back(new ScanThread(*this, ThreadType::TTPING));
				_threadsOther.push_back(new ScanThread(*this, ThreadType::TTOTHER));
				_threadsMain.push_back(new ScanThread(*this, ThreadType::TTMAIN, new wxSocketClient(wxSOCKET_WAITALL | wxSOCKET_BLOCK)));
			}
		}
	}

	if (!_started) {
		logger_base.debug("Starting work.");
		_started = true;
		for (const auto& it : _threadsMain) {
			it->Run();
		}
		for (const auto& it : _threadsPing) {
			it->Run();
		}
		for (const auto& it : _threadsOther) {
			it->Run();
		}
	}
}

void WorkManager::Stop()
{
	static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
	logger_base.debug("Stopping work");
	for (const auto& it : _threadsMain) {
		it->Terminate();
	}
	for (const auto& it : _threadsPing) {
		it->Terminate();
	}
	for (const auto& it : _threadsOther) {
		it->Terminate();
	}
	_started = false;
}

WorkManager::~WorkManager()
{
	while (_threadsMain.size() > 0) {
		ScanThread* t = _threadsMain.back();
		_threadsMain.pop_back();
		t->Terminate();
		t->Kill();
	}

	while (_threadsPing.size() > 0) {
		ScanThread* t = _threadsPing.back();
		_threadsPing.pop_back();
		t->Terminate();
		t->Kill();
	}

	while (_threadsOther.size() > 0) {
		ScanThread* t = _threadsOther.back();
		_threadsOther.pop_back();
		t->Terminate();
		t->Kill();
	}
}

wxThread::ExitCode ScanThread::Entry()
{
	if (_client != nullptr) _client->SetTimeout(3);

	while (!_terminate) {
		auto w = _workManager.GetWork(_threadType);
		if (w.has_value()) {
			{
				std::unique_lock<std::mutex> locker(_mutex);
				_activeWork = *w;
			}
			(*w)->DoWork(_workManager, _client);
			{
				std::unique_lock<std::mutex> locker(_mutex);
				_activeWork = nullptr;
			}
			delete (*w);
		}
		else {
			wxSleep(1);
		}
	}

	return 0;
}

void ScanThread::TerminateWork()
{
	std::unique_lock<std::mutex> locker(_mutex);
	if (_activeWork != nullptr) {
		_activeWork->Terminate();
	}
}

void WorkManager::AddHTTP(const std::string& ip, int port, const std::string& proxy)
{
	std::lock_guard<std::mutex> lock(_mutex);

	auto h = wxString::Format("%s:%s:%d", ip, proxy, port);

	if (std::find(begin(_scannedHTTP), end(_scannedHTTP), h) == end(_scannedHTTP)) {
		// start with a ping ... work flows from there
		_scannedHTTP.push_back(h);
		_queueMain.push(new HTTPWork(ip, port, proxy));
	}
}

void WorkManager::AddIP(const std::string& ip, const std::string& why, const std::string& proxy)
{
	std::lock_guard<std::mutex> lock(_mutex);

	if (std::find(begin(_scannedIP), end(_scannedIP), ip) == end(_scannedIP)) {
		// start with a ping ... work flows from there
		_scannedIP.push_back(ip);
		_queuePing.push(new PingWork(ip, why, proxy));
	}
}

void WorkManager::AddClassDSubnet(const std::string& ip, const std::string& proxy)
{
	wxArrayString ipElements = wxSplit(ip, '.');
	if (ipElements.size() > 3) {
		//looks like an IP address
		int ip1 = wxAtoi(ipElements[0]);
		int ip2 = wxAtoi(ipElements[1]);
		int ip3 = wxAtoi(ipElements[2]);
		int ip4 = wxAtoi(ipElements[3]);

		for (uint8_t i = 1; i < 255; i++) {
			auto ipa = wxString::Format("%d.%d.%d.%d", ip1, ip2, ip3, i);
			AddIP(ipa, "", proxy);
		}
	}
}

void PingWork::DoWork(WorkManager& workManager, wxSocketClient* client)
{
	static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));

	std::list<std::pair<std::string, std::string>> results;

	logger_base.debug("PingWork %s", (const char*)_ip.c_str());

	// First determine if public or private

	// We only scan private networks
	bool privateNetwork = true;

	wxArrayString ipElements = wxSplit(_ip, '.');
	if (ipElements.size() > 3) {
		//looks like an IP address
		int ip1 = wxAtoi(ipElements[0]);
		int ip2 = wxAtoi(ipElements[1]);
		int ip3 = wxAtoi(ipElements[2]);
		int ip4 = wxAtoi(ipElements[3]);

		if (ip1 == 10) {
			if (ip2 == 255 && ip3 == 255 && ip4 == 255) {
				// this is a broadcast network
				privateNetwork = false;
			}
			// else this is valid
		}
		else if (ip1 == 192 && ip2 == 168) {
			if (ip3 == 255 && ip4 == 255) {
				// this is a broadcast network
				privateNetwork = false;
			}
			// else this is valid
		}
		else if (ip1 == 172 && ip2 >= 16 && ip2 <= 31) {
			// this is valid
		}
		else if (ip1 == 255 && ip2 == 255 && ip3 == 255 && ip4 == 255) {
			// this is a broadcast network
			privateNetwork = false;
		}
		else if (ip1 == 0) {
			// this is an invalid betwork
			privateNetwork = false;
		}
		else if (ip1 >= 224 && ip1 <= 239) {
			// this is a multicast network
			privateNetwork = false;
		}
		else {
			// this is a routable network
			privateNetwork = false;
		}

		results.push_back({ "IP", _ip });
		results.push_back({ "Type", "Ping" });
		if (_why != "") {
			results.push_back({ "Why", _why });
		}
		results.push_back({ "Network", wxString::Format("%d.%d.%d.0", ip1, ip2, ip3)});
		results.push_back({ "Network Type", privateNetwork ? "Private" : "Public" });

		auto const& result = IPOutput::Ping(_ip, _proxy);
		if (result == IPOutput::PINGSTATE::PING_OK || result == IPOutput::PINGSTATE::PING_WEBOK) {
			results.push_back({ "PING", "OK" });
			workManager.AddFoundIP(_ip);
			workManager.AddHTTP(_ip, 80, _proxy);
			//workManager.AddHTTP(_ip, 81);
			//workManager.AddHTTP(_ip, 8080);
			//workManager.AddHTTP(_ip, 8081);
			PublishResult(workManager, results);
		}
		else {
			results.push_back({ "PING", "FAILED" });
			PublishResult(workManager, results);
			workManager.AddHTTP(_ip, 80, _proxy); // we still try to open as it may be behind a http proxy
		}
	}
}

std::string HTTPWork::GetTitle()
{
	try {
		std::string page = Curl::HTTPSGet(_proxy + _ip, "", "", SLOW_TIMEOUT);

		if (page == "") {
			return "";
		}

		wxRegEx title("<title>([^<]*)<");
		if (title.Matches(page)) {
			wxString t = title.GetMatch(page, 1);

			if (t.Contains("404")) {
				return "";
			}

			return t;
		}
	}
	catch (...) 		{

	}

	return "";
}

void HTTPWork::DoWork(WorkManager& workManager, wxSocketClient* client)
{
	static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));

	std::list<std::pair<std::string, std::string>> results;

	logger_base.debug("HTTPWork %s:%d", (const char*)_ip.c_str(), _port);

	wxIPV4address addr;
	addr.Hostname(_ip);
	addr.Service(_port);

	if (client != nullptr) {
		if (client->Connect(addr)) {
			client->Close();
			logger_base.debug("    HTTP Connected.");
			results.push_back({ "IP", _ip });
			results.push_back({ "Type", "HTTP" });
			results.push_back({ "Port", wxString::Format("%d", _port)});
			results.push_back({ "Web", "OK" });

			std::string title = GetTitle();
			if (title != "") {
				results.push_back({ "Title", title });
			}
			PublishResult(workManager, results);

			workManager.AddWork(new FPPWork(_ip, _proxy));
			workManager.AddWork(new FalconWork(_ip, _proxy));
			workManager.AddWork(new xScheduleWork(_ip));
		}
		else {
			logger_base.debug("    HTTP Connect failed.");
		}
	}
	else {
		wxASSERT(false);
	}
}

void FPPWork::DoWork(WorkManager& workManager, wxSocketClient* client)
{
	static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));

	std::list<std::pair<std::string, std::string>> results;

	std::string proxy;
	if (_proxy != "") {
		proxy = _proxy + "/proxy/";
	}

	logger_base.debug("FPPWork %s %s", (const char*)_proxy.c_str(), (const char*)_ip.c_str());
	auto netconfig = Curl::HTTPSGet(proxy + _ip + "/api/network/interface", "", "", FAST_TIMEOUT);

	if (netconfig != "" && Contains(netconfig, "operstate")) {

		logger_base.debug("    FPP found");
		results.push_back({ "IP", _ip });
		results.push_back({ "Type", "FPP" });

		logger_base.debug("    Getting wifi strength");
		auto wificonfig = Curl::HTTPSGet(proxy + _ip + "/api/network/wifi/strength", "", "", FAST_TIMEOUT);
		if (wificonfig != "") {

			wxJSONReader wifireader;
			wxJSONValue wifiroot;
			bool fwifi = wifireader.Parse(wificonfig, &wifiroot) == 0;

			wxJSONValue defaultValue = wxString("");
			wxJSONReader reader;
			wxJSONValue root;
			if (reader.Parse(netconfig, &root) == 0) {
				// extract the type of request
				auto net = root.AsArray();

				if (net != nullptr) {
					int ii = 1;
					for (size_t i = 0; i < net->Count(); i++) {
						auto n = (*net)[i];
						wxString operstate = n.Get("operstate", defaultValue).AsString();
						wxString iip = n.Get("addr_info", defaultValue)[0].Get("local", defaultValue).AsString();
						wxString label = n.Get("addr_info", defaultValue)[0].Get("label", defaultValue).AsString();
						if (operstate == "UP" && iip != "" && label != "") {
							wxString wifiStrength = "";
							if (label[0] == 'w' && fwifi) {
								auto w = wifiroot.AsArray();
								for (size_t j = 0; j < w->Count(); j++) {
									auto ww = (*w)[j];
									wxString iface = ww.Get("interface", defaultValue).AsString();
									if (iface == label) {
										int strength = ww.Get("level", wxJSONValue(0)).AsInt();
										wifiStrength = wxString::Format(" (%d - %s)", strength, DecodeWifiStrength(strength));
										break;
									}
								}
							}
							results.push_back({ wxString::Format("IP %d", ii++), label + " : " + iip + " " + wifiStrength });
							workManager.AddIP(iip, "");
						}
					}
				}
			}
		}

		logger_base.debug("    Getting FPP proxies");
		auto proxies = Curl::HTTPSGet(proxy + _ip + "/api/proxies", "", "", FAST_TIMEOUT);
		if (proxies != "" && proxies != "[]") {
			wxJSONValue defaultValue = wxString("");
			wxJSONReader reader;
			wxJSONValue root;
			if (reader.Parse(proxies, &root) == 0) {
				if (root.IsArray()) {
					auto ps = root.AsArray();
					if (ps != nullptr) {
						int c = 1;
						for (size_t i = 0; i < ps->Count(); i++) {
							if ((*ps)[i].IsString()) {
								wxString p = (*ps)[i].AsString();
								results.push_back({ wxString::Format("Proxying %d", c++), p });
								workManager.AddIP(p, "FPP Proxied", _ip);
								workManager.AddClassDSubnet(p, _ip);
								std::list<std::pair<std::string, std::string>> pres;
								pres.push_back({ "Type", "Proxied" });
								pres.push_back({ "IP", p });
								pres.push_back({ "Proxied By", _ip });
								PublishResult(workManager, pres);
							}
						}
					}
				}
			}
		}

		logger_base.debug("    Getting FPP version");
		auto version = Curl::HTTPSGet(proxy + _ip + "/api/fppd/version", "", "", FAST_TIMEOUT);
		if (version != "" && version[0] == '{') {
			wxJSONValue defaultValue = wxString("");
			wxJSONReader reader;
			wxJSONValue root;
			if (reader.Parse(version, &root) == 0) {
				results.push_back({ "Version", root.Get("version", defaultValue).AsString() });
			}
		}

		logger_base.debug("    Getting FPP Channel Outputs");
		auto co = Curl::HTTPSGet(proxy + _ip + "/api/configfile/co-universes.json", "", "", FAST_TIMEOUT);
		if (co != "" && co[0] == '{') {
			wxJSONValue defaultValue = wxString("");
			wxJSONReader reader;
			wxJSONValue root;
			if (reader.Parse(co, &root) == 0) {
				auto cc = root.Get("channelOutputs", defaultValue);
				if (cc.IsArray() && cc[0].Get("enabled", defaultValue).AsInt() == 1) {
					results.push_back({ "Sending Data", cc[0].Get("interface", defaultValue).AsString() });
				}
			}
		}

		logger_base.debug("    Getting FPP status");
		auto status = Curl::HTTPSGet(proxy + _ip + "/api/fppd/status", "", "", FAST_TIMEOUT);
		if (status != "" && status[0] == '{') {
			wxJSONValue defaultValue = wxString("");
			wxJSONReader reader;
			wxJSONValue root;
			if (reader.Parse(status, &root) == 0) {
				results.push_back({ "Mode", root.Get("mode_name", defaultValue).AsString() });
			}
		}

		logger_base.debug("    Getting FPP multisync");
		auto multisync = Curl::HTTPSGet(proxy + _ip + "/api/fppd/multiSyncSystems", "", "", FAST_TIMEOUT);
		if (multisync != "" && multisync[0] == '{') {
			wxJSONValue defaultValue = wxString("");
			wxJSONReader reader;
			wxJSONValue root;
			if (reader.Parse(multisync, &root) == 0) {
				auto sys = root.Get("systems", defaultValue).AsArray();

				if (sys != nullptr) {
					for (size_t i = 0; i < sys->Count(); i++) {
						auto s = (*sys)[i];
						std::string address = s.Get("address", defaultValue).AsString().ToStdString();
						workManager.AddIP(address, "FPP Multisync");
					}
				}
			}
		}
		PublishResult(workManager, results);
	}
	else {
		logger_base.debug("    Not FPP");
	}
}

void FalconWork::DoWork(WorkManager& workManager, wxSocketClient* client)
{
	static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));

	std::list<std::pair<std::string, std::string>> results;

	std::string proxy;
	if (_proxy != "") {
		proxy = _proxy + "/proxy/";
	}

	logger_base.debug("FalconWork %s %s", (const char*)_proxy.c_str(), (const char*)_ip.c_str());
	auto status = Curl::HTTPSGet(proxy + _ip + "/status.xml", "", "", SLOW_TIMEOUT);

	if (status != "" && Contains(status, "<response>") && Contains(status, "<fv>")) {

		logger_base.debug("    Falcon found");
		results.push_back({ "IP", _ip});
		results.push_back({ "Type", "Falcon" });

		wxXmlDocument doc;
		wxStringInputStream docstrm(status);
		doc.Load(docstrm);
		if (doc.IsOk() && doc.GetRoot() != nullptr) {
			int k0 = 0;
			int k1 = 0;
			int k2 = 0;
			int p = 0;
			for (auto n = doc.GetRoot()->GetChildren(); n != nullptr; n = n->GetNext()) {
				if (n->GetChildren() != nullptr) {
					if (n->GetName() == "m") {
						int m = wxAtoi(n->GetChildren()->GetContent());
						results.push_back({ "Mode", Falcon::DecodeMode(m) });
					}
					else if (n->GetName() == "k0") {
						k0 = wxAtoi(n->GetChildren()->GetContent());
					}
					else if (n->GetName() == "k1") {
						k1 = wxAtoi(n->GetChildren()->GetContent());
					}
					else if (n->GetName() == "k2") {
						k2 = wxAtoi(n->GetChildren()->GetContent());
					}
					else if (n->GetName() == "p") {
						p = wxAtoi(n->GetChildren()->GetContent());
					}
					else if (n->GetName() == "fv") {
						results.push_back({ "Firmware Version", n->GetChildren()->GetContent() });
					}
					else if (n->GetName() == "n") {
						results.push_back({ "Name", n->GetChildren()->GetContent() });
					}
				}
			}
			if (k0 != 0 || k1 != 0 || k2 != 0) {
				results.push_back({ "Banks", wxString::Format("%d:%d:%d", k0, k1, k2) });
			}

			int model;
			int version;
			Falcon::DecodeModelVersion(p, model, version);

			if (p == 128) {
				Falcon falcon(_ip, _proxy);
				if (falcon.IsConnected()) {
					wxJSONValue status = falcon.V4_GetStatus();
					if (status.HasMember("O")) results.push_back({ "Mode", falcon.V4_DecodeMode(status["O"].AsInt()) });
					if (status.HasMember("B") && status["WI"].AsString() != "") {
						results.push_back({ "WIFI IP", "WIFI: " + status["WI"].AsString() + " : " + status["WK"].AsString() + " : " + status["WS"].AsString() });
						workManager.AddIP(status["WI"].AsString(), "");
					}
					if (status.HasMember("I") && status["I"].AsString() != "") {
						results.push_back({ "ETH IP", "Wired: " + status["I"].AsString() + " : " + status["K"].AsString() });
						workManager.AddIP(status["I"].AsString(), "");
					}
					results.push_back({ "Model", wxString::Format("F%dv4", status["BR"].AsInt()).ToStdString() });
					if (status.HasMember("TS") && status["TS"].AsInt() != 0) {
						results.push_back({ "Test Mode", "Enabled" });
					}
					if (status.HasMember("N")) results.push_back({ "Name", status["N"].AsString()});
					if (status.HasMember("T1")) results.push_back({ "Temp1", wxString::Format("%.1fC", (float)status["T1"].AsInt() / 10.0).ToStdString() });
					if (status.HasMember("T2")) results.push_back({ "Temp2", wxString::Format("%.1fC", (float)status["T2"].AsInt() / 10.0).ToStdString() });
					if (status.HasMember("PT")) results.push_back({ "Processor Temp", wxString::Format("%.1fC", (float)status["PT"].AsInt() / 10.0).ToStdString() });
					if (status.HasMember("FN")) results.push_back({ "Fan Speed", wxString::Format("%d RPM", status["FN"].AsInt()).ToStdString() });
					if (status.HasMember("V1")) results.push_back({ "V1", wxString::Format("%.1fV", (float)status["V1"].AsInt() / 10.0).ToStdString() });
					if (status.HasMember("V2")) results.push_back({ "V2", wxString::Format("%.1fV", (float)status["V2"].AsInt() / 10.0).ToStdString() });
					if (status.HasMember("B")) results.push_back({"Board Configuration", falcon.V4_DecodeBoardConfiguration(status["B"].AsInt())});
				}
			}
			else {
				results.push_back({ "Model", wxString::Format("F%dv%d", model, version).ToStdString() });
			}
		}
		PublishResult(workManager, results);
	}
}

wxWindow* ScanWork::GetFrameWindow()
{
	return wxGetApp().GetTopWindow();
}

void ScanWork::PublishResult(WorkManager& workManager, std::list<std::pair<std::string, std::string>>& result)
{
	std::list<std::pair<std::string, std::string>> out;

	if (result.size() == 0) return;

	// now deduplicate any results by copying back to front ... this allows work to add the same vallue multiple times with the last value winning
	for (auto it = result.rbegin(); it != result.rend(); ++it) 		{
		bool found = false;
		for (const auto& it2 : out) 			{
			if (it2.first == it->first) {
				found = true;
				break;
			}
		}
		if (!found) 			{
			out.push_front(*it);
		}
	}

	workManager.PublishResult(out);
}

void xScheduleWork::DoWork(WorkManager& workManager, wxSocketClient* client)
{
	static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));

	logger_base.debug("xScheduleWork %s:%d", (const char*)_ip.c_str(), _port);

	std::list<std::pair<std::string, std::string>> results;

	auto xs = Curl::HTTPSGet(_ip + ":" + wxString::Format("%d", _port) + "/xScheduleQuery?Query=getplayingstatus", "", "", FAST_TIMEOUT);
	if (xs != "" && xs[0] == '{') {
		logger_base.debug("    xSchedule found");
		results.push_back({ "IP", _ip });
		results.push_back({ "Type", "xSchedule" });
		results.push_back({ "Port", wxString::Format("%d", _port) });
		wxJSONValue defaultValue = wxString("");
		wxJSONReader reader;
		wxJSONValue root;
		if (reader.Parse(xs, &root) == 0) {
			results.push_back({ "Version", root.Get("version", defaultValue).AsString() });
		}
		PublishResult(workManager, results);
	}
}

void DiscoverWork::DoWork(WorkManager& workManager, wxSocketClient* client)
{
	static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));

	logger_base.debug("DiscoverWork");

	OutputManager om;
	Discovery discovery(GetFrameWindow(), &om);

	Pixlite16::PrepareDiscovery(discovery);
	ZCPPOutput::PrepareDiscovery(discovery);
	ArtNetOutput::PrepareDiscovery(discovery);
	DDPOutput::PrepareDiscovery(discovery);
	FPP::PrepareDiscovery(discovery);

	discovery.Discover();

	for (size_t x = 0; x < discovery.GetResults().size(); x++) {
		auto discovered = discovery.GetResults()[x];
		if (discovered == nullptr || discovered->controller == nullptr) {
			continue;
		}
		ControllerEthernet* it = discovered->controller;

		if (it != nullptr && it->GetResolvedIP() != "") {
			std::list<std::pair<std::string, std::string>> results;
			results.push_back({ "Type", "Discover" });
			results.push_back({ "IP", it->GetResolvedIP() });
			results.push_back({ "Discovered", "TRUE" });
			if (discovered->vendor != "") {
				results.push_back({ "Vendor", discovered->vendor });
			}
			if (discovered->model != "") {
				results.push_back({ "Model", discovered->model });
			}
			if (discovered->platform != "") {
				results.push_back({ "Platform", discovered->platform });
			}
			if (discovered->platformModel != "") {
				results.push_back({ "Platform Model", discovered->platformModel });
			}
			if (discovered->hostname != "" && (discovered->hostname[0] < '0' || discovered->hostname[0] > '9')) {
				results.push_back({ "Name", discovered->hostname });
			}
			if (discovered->version != "") {
				results.push_back({ "Version", discovered->version });
			}
			if (discovered->mode != "") {
				results.push_back({ "Mode", discovered->mode });
			}
			PublishResult(workManager, results);

			workManager.AddHTTP(it->GetResolvedIP(), 80);
			workManager.AddWork(new FalconWork(it->GetResolvedIP(), discovered->proxy));
			workManager.AddWork(new FPPWork(it->GetResolvedIP(), discovered->proxy));
			workManager.AddIP(it->GetResolvedIP(), "Discover");
			workManager.AddClassDSubnet(it->GetResolvedIP());
		}
	}
}

void ComputerWork::ScanARP(WorkManager& workManager)
{
	static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));
	std::map<std::string, std::string> arps;

#ifdef __WXMSW__
	logger_base.debug("Reading ARP table");
	DWORD bytesNeeded = 0;
	PMIB_IPNETTABLE arp = nullptr;

	// The result from the API call.
	int result = ::GetIpNetTable(nullptr, &bytesNeeded, false);

	// Call the function, expecting an insufficient buffer.
	if (result == ERROR_INSUFFICIENT_BUFFER) {

		arp = (PMIB_IPNETTABLE)malloc(bytesNeeded);
		if (arp != nullptr) {
			result = ::GetIpNetTable(arp, &bytesNeeded, false);

			if (result == 0) {
				for (size_t i = 0; i < arp->dwNumEntries; i++) {
					auto a = arp->table[i];

					std::string iip;
					std::string mac;

					if (memcmp(a.bPhysAddr, "\0\0\0\0\0\0\0\0", 8) != 0) {
						if (((a.dwAddr >> 24) & 0xFF) != 0xFF) {
							mac = wxString::Format("%02X-%02X-%02X-%02X-%02X-%02X", a.bPhysAddr[0], a.bPhysAddr[1], a.bPhysAddr[2], a.bPhysAddr[3], a.bPhysAddr[4], a.bPhysAddr[5]);
							iip = wxString::Format("%d.%d.%d.%d", a.dwAddr & 0xFF, (a.dwAddr >> 8) & 0xFF, (a.dwAddr >> 16) & 0xFF, (a.dwAddr >> 24) & 0xFF);
							arps[iip] = mac;
						}
					}
				}
			}
			free(arp);
		}
	}
#endif

#ifdef __LINUX__
	logger_base.debug("Reading ARP table");
	const int size = 256;

	char ip_address[size];
	int hw_type;
	int flags;
	char mac_address[size];
	char mask[size];
	char device[size];

	FILE* fp = fopen("/proc/net/arp", "r");
	if (fp != NULL) {
		char line[size];
		fgets(line, size, fp);    // Skip the first line, which consists of column headers.
		while (fgets(line, size, fp)) {
			sscanf(line, "%s 0x%x 0x%x %s %s %s\n",
				ip_address,
				&hw_type,
				&flags,
				mac_address,
				mask,
				device);
			std::string iip(ip_address);
			std::string mac(mac_address);
			arps[iip] = mac;
		}
	}
	else {
		logger_base.error("Error Reading ARP table");
	}
	fclose(fp);
#endif

	for (const auto& it : workManager.GetFound()) {
		if (arps.find(it) == end(arps)) {
			// no mac address
		}
		else {
			// we only process them once
			if (std::find(begin(_macsDone), end(_macsDone), arps[it]) == end(_macsDone)) {
				workManager.AddWork(new MACWork(it, arps[it]));
				_macsDone.push_back(arps[it]);
			}
		}
	}

	for (const auto& it : arps) {
		workManager.AddIP(it.first, "ARP");
	}
}

std::string ComputerWork::GetXLightsShowFolder()
{
	wxString showDir = "";

	wxConfig* xlconfig = new wxConfig(_("xLights"));
	if (xlconfig != nullptr) {
		xlconfig->Read(_("LastDir"), &showDir);
		delete xlconfig;
	}

	return showDir.ToStdString();
}

std::string ComputerWork::GetXScheduleShowFolder()
{
	wxString showDir = "";

	wxConfig* xlconfig = new wxConfig(_("xSchedule"));
	if (xlconfig != nullptr) {
		xlconfig->Read(_("SchedulerLastDir"), &showDir);
		delete xlconfig;
	}

	return showDir.ToStdString();
}

std::string ComputerWork::GetForceIP()
{
	wxString localIP = "";

	wxConfig* xlconfig = new wxConfig(_("xLights"));
	if (xlconfig != nullptr) {
		xlconfig->Read(_("xLightsLocalIP"), &localIP, "");
		delete xlconfig;
	}

	return localIP.ToStdString();
}

void ComputerWork::ProcessController(WorkManager& workManager, Controller* controller, const std::string& why)
{
	//static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));

	std::list<std::pair<std::string, std::string>> results;

	auto proxy = controller->GetFPPProxy();
	auto ip = controller->GetResolvedIP();

	if (ip != "") 		{	
		results.push_back({ "Type", "Controller" });
		results.push_back({ "IP", ip });
		results.push_back({ "Why", why + " Controller"});
		results.push_back({ "Name", controller->GetName() });
		results.push_back({ "Vendor", controller->GetVendor() });
		results.push_back({ "Model", controller->GetModel() });
		results.push_back({ "Variant", controller->GetVariant() });
		switch (controller->GetActive()) 			{
		case Controller::ACTIVESTATE::ACTIVE:
			results.push_back({ "Active", "Active" });
			break;
		case Controller::ACTIVESTATE::ACTIVEINXLIGHTSONLY:
			results.push_back({ "Active", "xLights Only" });
			break;
		case Controller::ACTIVESTATE::INACTIVE:
			results.push_back({ "Active", "Inactive" });
			break;
		}
		results.push_back({ "Description", controller->GetDescription() });
		results.push_back({ "Protocol", controller->GetColumn1Label() });
		results.push_back({ "Universes/Id", controller->GetColumn3Label() });
		results.push_back({ "Channels", controller->GetColumn4Label() });

		workManager.AddIP(ip, why + " Controller", proxy);
		workManager.AddHTTP(ip, 80, proxy);
		workManager.AddWork(new FalconWork(ip, proxy));
		workManager.AddWork(new FPPWork(ip, proxy));
		workManager.AddClassDSubnet(ip, proxy);

		PublishResult(workManager, results);
	}
}

void ComputerWork::DoWork(WorkManager& workManager, wxSocketClient* client)
{
	static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));

	std::list<std::pair<std::string, std::string>> results;

	logger_base.debug("ComputerWork:");

	results.push_back({ "Type", "Computer" });

	results.push_back({ "Computer Name", wxGetHostName() });

	if (GetForceIP() != "") 		{
		results.push_back({ "Force Local IP", GetForceIP() });
	}

	_xLightsShowFolder = GetXLightsShowFolder();
	if (_xLightsShowFolder != "") {
		results.push_back({ "xLights Show Folder", _xLightsShowFolder });

		OutputManager om;
		om.Load(_xLightsShowFolder, false);

		if (om.GetGlobalFPPProxy() != "") 			{
			results.push_back({ "xLights Global FPP Proxy", om.GetGlobalFPPProxy() });
			workManager.AddIP(om.GetGlobalFPPProxy(), "xLights Global FPP Proxy");
		}

		for (const auto& it : om.GetControllers()) {
			ProcessController(workManager, it, "xLights");
		}
	}
	_xScheduleShowFolder = GetXScheduleShowFolder();
	if (_xScheduleShowFolder != "" && _xLightsShowFolder != _xScheduleShowFolder) {
		results.push_back({ "xSchedule Show Folder", _xScheduleShowFolder });

		OutputManager om;
		om.Load(_xScheduleShowFolder, false);

		if (om.GetGlobalFPPProxy() != "") {
			results.push_back({ "xSchedule Global FPP Proxy", om.GetGlobalFPPProxy() });
			workManager.AddIP(om.GetGlobalFPPProxy(), "xSchedule Global FPP Proxy");
		}

		for (const auto& it : om.GetControllers()) {
			ProcessController(workManager, it, "xSchedule");
		}
	}

	workManager.AddWork(new DiscoverWork());

	auto localIPs = GetLocalIPs();
	int i = 1;
	for (const auto& it : localIPs) {
		if (it != "127.0.0.1") {
			workManager.AddHTTP(it, 80);
			workManager.AddWork(new xScheduleWork(it, 80));
			workManager.AddWork(new xScheduleWork(it, 81));
			workManager.AddWork(new xScheduleWork(it, 8080));
			workManager.AddWork(new xScheduleWork(it, 8081));
			workManager.AddClassDSubnet(it);
			results.push_back({ wxString::Format("Local IP %d", i++), it });
		}
	}

	// read any static routes
#ifdef __WXMSW__
	DWORD dwSize = 0;
	if (::GetIpForwardTable(nullptr, &dwSize, true) == ERROR_INSUFFICIENT_BUFFER) {
		MIB_IPFORWARDTABLE* p = (MIB_IPFORWARDTABLE*)malloc(dwSize);
		if (p != nullptr) {
			if (::GetIpForwardTable(p, &dwSize, true) == NO_ERROR) {
				for (int i = 0; i < (int)p->dwNumEntries; i++) {
					if (p->table[i].dwForwardProto == MIB_IPPROTO_NETMGMT && (u_long)p->table[i].dwForwardDest != 0) {
						char szDestIp[128];
						//char szMaskIp[128];
						struct in_addr IpAddr;
						IpAddr.S_un.S_addr = (u_long)p->table[i].dwForwardDest;
						strcpy_s(szDestIp, sizeof(szDestIp), inet_ntoa(IpAddr));
						//IpAddr.S_un.S_addr = (u_long)p->table[i].dwForwardMask;
						//strcpy_s(szMaskIp, sizeof(szMaskIp), inet_ntoa(IpAddr));
						auto ip = std::string(szDestIp);
						workManager.AddClassDSubnet(ip);
						results.push_back({ wxString::Format("Static Route %d", i + 1), ip });
					}
				}
			}
			free(p);
		}
	}
#endif

	PublishResult(workManager, results);

	for (int i = 0; i < 40 && !_terminate; i++) {
		for (uint32_t j = 0; j < 1500 && !_terminate; j++) {
			wxMilliSleep(10);
		}
		if (!_terminate) {
			ScanARP(workManager);
		}
	}
}

void MACWork::DoWork(WorkManager& workManager, wxSocketClient* client)
{
	static log4cpp::Category& logger_base = log4cpp::Category::getInstance(std::string("log_base"));

	static std::mutex lockcache;
	static std::map<std::string, std::string> cache; // because people tend to have multiple from the same vendor ... cache what we find
	std::list<std::pair<std::string, std::string>> results;

	logger_base.debug("MACWork: %s", (const char*)_mac.c_str());

	auto vendor = LookupMacAddress(_mac);

	if (vendor == "") {
		std::unique_lock<std::mutex> locker(lockcache);
		if (cache.find(_mac) != end(cache)) {
			vendor = cache[_mac];
		}
	}

	if (vendor == "") {
		auto macURL = std::string("https://api.macvendors.com/" + _mac);
		logger_base.debug("    Looking up MAC: %s", (const char*)macURL.c_str());
		vendor = Curl::HTTPSGet(macURL, "", "", SLOW_TIMEOUT);
		if (Contains(vendor, "\"Not Found\"")) {
			vendor = "";
		}
		else if (Contains(vendor, "\"Too Many Requests\"")) {
			vendor = "MAC Lookup Unavailable";
		}

		if (vendor != "MAC Lookup Unavailable") {
			std::unique_lock<std::mutex> locker(lockcache);
			cache[_mac] = vendor;
		}
	}

	if (vendor != "") 		{
		results.push_back({ "Type", "MAC" });
		results.push_back({ "IP", _ip });
		results.push_back({ "MAC", _mac });
		results.push_back({ "MAC Vendor", vendor });
		PublishResult(workManager, results);
	}
	else 		{
		results.push_back({ "Type", "MAC" });
		results.push_back({ "IP", _ip });
		results.push_back({ "MAC", _mac });
		PublishResult(workManager, results);
	}
}
