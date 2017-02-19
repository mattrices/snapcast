/***
    This file is part of snapcast
    Copyright (C) 2014-2017  Johannes Pohl

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
***/

#include "streamServer.h"
#include "message/time.h"
#include "message/hello.h"
#include "common/log.h"
#include "config.h"
#include <iostream>

using namespace std;

using json = nlohmann::json;


StreamServer::StreamServer(asio::io_service* io_service, const StreamServerSettings& streamServerSettings) : io_service_(io_service), settings_(streamServerSettings)
{
}


StreamServer::~StreamServer()
{
}


void StreamServer::onStateChanged(const PcmStream* pcmStream, const ReaderState& state)
{
	logO << "onStateChanged (" << pcmStream->getName() << "): " << state << "\n";
//	logO << pcmStream->toJson().dump(4);
	json notification = jsonrpcpp::Notification("Stream.OnUpdate", jsonrpcpp::Parameter("id", pcmStream->getId(), "stream", pcmStream->toJson())).to_json();
	controlServer_->send(notification.dump(), NULL);
	cout << "Notification:\n" << notification.dump(4) << "\n";
}


void StreamServer::onChunkRead(const PcmStream* pcmStream, const msg::PcmChunk* chunk, double duration)
{
//	logO << "onChunkRead (" << pcmStream->getName() << "): " << duration << "ms\n";
	bool isDefaultStream(pcmStream == streamManager_->getDefaultStream().get());

	std::shared_ptr<const msg::BaseMessage> shared_message(chunk);
	std::lock_guard<std::recursive_mutex> mlock(sessionsMutex_);
	for (auto s : sessions_)
	{
		if (!s->pcmStream() && isDefaultStream)//->getName() == "default")
			s->sendAsync(shared_message);
		else if (s->pcmStream().get() == pcmStream)
			s->sendAsync(shared_message);
	}
}


void StreamServer::onResync(const PcmStream* pcmStream, double ms)
{
	logO << "onResync (" << pcmStream->getName() << "): " << ms << "ms\n";
}


void StreamServer::onDisconnect(StreamSession* streamSession)
{
	std::lock_guard<std::recursive_mutex> mlock(sessionsMutex_);
	session_ptr session = getStreamSession(streamSession);

	if (session == nullptr)
		return;

	logO << "onDisconnect: " << session->clientId << "\n";
	logD << "sessions: " << sessions_.size() << "\n";
	// don't block: remove StreamSession in a thread
	auto func = [](shared_ptr<StreamSession> s)->void{s->stop();};
	std::thread t(func, session);
	t.detach();
	sessions_.erase(session);

	logD << "sessions: " << sessions_.size() << "\n";

	// notify controllers if not yet done
	ClientInfoPtr clientInfo = Config::instance().getClientInfo(streamSession->clientId);
	if (!clientInfo || !clientInfo->connected)
		return;

	clientInfo->connected = false;
	gettimeofday(&clientInfo->lastSeen, NULL);
	Config::instance().save();
	if (controlServer_ != nullptr)
	{
		json notification = jsonrpcpp::Notification("Client.OnDisconnect", jsonrpcpp::Parameter("id", clientInfo->id, "client", clientInfo->toJson())).to_json();
		controlServer_->send(notification.dump());
		cout << "Notification:\n" << notification.dump(4) << "\n";
	}
}


void StreamServer::ProcessRequest(const jsonrpcpp::request_ptr request, jsonrpcpp::entity_ptr& response, jsonrpcpp::notification_ptr& notification) const
{
	try
	{
		logO << "StreamServer::ProcessRequest method: " << request->method << ", " << "id: " << request->id << "\n";

		Json result;

		if (request->method.find("JSONRPC.") == 0)
		{
			//TODO: JSONRPC.Version, JSONRPC.Ping
		}
		else if (request->method.find("Client.") == 0)
		{
			ClientInfoPtr clientInfo = Config::instance().getClientInfo(request->params.get("id"));
			if (clientInfo == nullptr)
				throw jsonrpcpp::InternalErrorException("Client not found", request->id);

			if (request->method == "Client.GetStatus")
			{
				result = clientInfo->toJson();
			}
			else if (request->method == "Client.SetVolume")
			{
				/// -> {"jsonrpc":"2.0","method":"Client.SetVolume","id":5,"params":{"id":"00:21:6a:7d:74:fc#2","volume":{"percent":56,"muted":false}}}
				/// <- {"id":5,"jsonrpc":"2.0","result":{"volume":{"muted":false,"percent":56}}}
				clientInfo->config.volume.fromJson(request->params.get("volume"));
				result["volume"] = clientInfo->config.volume.toJson();
				notification.reset(new jsonrpcpp::Notification("Client.OnVolumeChanged", jsonrpcpp::Parameter("id", clientInfo->id, "volume", clientInfo->config.volume.toJson())));
			}
			else if (request->method == "Client.SetLatency")
			{
				/// -> {"jsonrpc":"2.0","method":"Client.SetLatency","id":4,"params":{"id":"00:21:6a:7d:74:fc#2","latency":50}}
				/// <- {"id":4,"jsonrpc":"2.0","result":{"latency":50}}
				int latency = request->params.get("latency");
				if (latency < -10000)
					latency = -10000;
				else if (latency > settings_.bufferMs)
					latency = settings_.bufferMs;
				clientInfo->config.latency = latency; //, -10000, settings_.bufferMs);
				result["latency"] = clientInfo->config.latency;
				notification.reset(new jsonrpcpp::Notification("Client.OnLatencyChanged", jsonrpcpp::Parameter("id", clientInfo->id, "latency", clientInfo->config.latency)));
			}
			else if (request->method == "Client.SetName")
			{
				/// -> {"jsonrpc":"2.0","method":"Client.SetName","id":3,"params":{"id":"00:21:6a:7d:74:fc#2","name":"test"}}
				/// <- {"id":3,"jsonrpc":"2.0","result":{"name":"test"}}
				clientInfo->config.name = request->params.get("name");
				result["name"] = clientInfo->config.name;
				notification.reset(new jsonrpcpp::Notification("Client.OnNameChanged", jsonrpcpp::Parameter("id", clientInfo->id, "name", clientInfo->config.name)));
			}
			else
				throw jsonrpcpp::MethodNotFoundException(request->id);


			if (request->method.find("Client.Set") == 0)
			{
				/// Update client
				session_ptr session = getStreamSession(clientInfo->id);
				if (session != nullptr)
				{
					msg::ServerSettings serverSettings;
					serverSettings.setBufferMs(settings_.bufferMs);
					serverSettings.setVolume(clientInfo->config.volume.percent);
					GroupPtr group = Config::instance().getGroupFromClient(clientInfo);
					serverSettings.setMuted(clientInfo->config.volume.muted || group->muted);
					serverSettings.setLatency(clientInfo->config.latency);
					session->send(&serverSettings);
				}
			}
		}
		else if (request->method.find("Group.") == 0)
		{
			GroupPtr group = Config::instance().getGroup(request->params.get("id"));
			if (group == nullptr)
				throw jsonrpcpp::InternalErrorException("Group not found", request->id);

			if (request->method == "Group.GetStatus")
			{
				result = group->toJson();
			}
			else if (request->method == "Group.SetMute")
			{
				/// -> {"jsonrpc":"2.0","method":"Group.SetMute","id":3,"params":{"id":"296ddff1-56fd-f2e4-232e-62c44faba7aa","mute":true}}
				/// <- {"id":3,"jsonrpc":"2.0","result":{"mute":true}}
				bool muted = request->params.get<bool>("mute");
				group->muted = muted;				

				/// Update clients
				for (auto client: group->clients)
				{
					session_ptr session = getStreamSession(client->id);
					if (session != nullptr)
					{
						msg::ServerSettings serverSettings;
						serverSettings.setBufferMs(settings_.bufferMs);
						serverSettings.setVolume(client->config.volume.percent);
						GroupPtr group = Config::instance().getGroupFromClient(client);
						serverSettings.setMuted(client->config.volume.muted || group->muted);
						serverSettings.setLatency(client->config.latency);
						session->send(&serverSettings);
					}
				}

				result["mute"] = group->muted;
				notification.reset(new jsonrpcpp::Notification("Group.OnMute", jsonrpcpp::Parameter("id", group->id, "mute", group->muted)));
			}
			else if (request->method == "Group.SetStream")
			{
				/// -> {"jsonrpc":"2.0","method":"Group.SetStream","id":1,"params":{"id":"296ddff1-56fd-f2e4-232e-62c44faba7aa","stream_id":"stream 2"}}
				/// <- {"id":1,"jsonrpc":"2.0","result":{"stream_id":"stream 2"}}
				string streamId = request->params.get("stream_id");
				PcmStreamPtr stream = streamManager_->getStream(streamId);
				if (stream == nullptr)
					throw jsonrpcpp::InternalErrorException("Stream not found", request->id);

				group->streamId = streamId;

				/// Update clients
				for (auto client: group->clients)
				{
					session_ptr session = getStreamSession(client->id);
					if (session && (session->pcmStream() != stream))
					{
						session->sendAsync(stream->getHeader());
						session->setPcmStream(stream);
					}
				}

				/// Notify others
				result["stream_id"] = group->streamId;
				notification.reset(new jsonrpcpp::Notification("Group.OnStreamChanged", jsonrpcpp::Parameter("id", group->id, "stream_id", group->streamId)));
			}
			else if (request->method == "Group.SetClients")
			{
				/// -> {"jsonrpc":"2.0","method":"Group.SetClients","id":6,"params":{"id":"25d719fd-3a4b-2090-2ac4-b17f62b7b18b","clients":["00:21:6a:7d:74:fc","00:21:6a:7d:74:fc#2"]}}
				/// <- {"id":6,"jsonrpc":"2.0","result":{"server":{"groups":[{"clients":[{"config":{"instance":1,"latency":0,"name":"","volume":{"muted":false,"percent":100}},"connected":true,"host":{"arch":"x86_64","ip":"192.168.0.54","mac":"00:21:6a:7d:74:fc","name":"T400","os":"Linux Mint 17.3 Rosa"},"id":"00:21:6a:7d:74:fc","lastSeen":{"sec":1487521394,"usec":253219},"snapclient":{"name":"Snapclient","protocolVersion":2,"version":"0.10.0"}},{"config":{"instance":2,"latency":50,"name":"test","volume":{"muted":false,"percent":56}},"connected":true,"host":{"arch":"x86_64","ip":"127.0.0.1","mac":"00:21:6a:7d:74:fc","name":"T400","os":"Linux Mint 17.3 Rosa"},"id":"00:21:6a:7d:74:fc#2","lastSeen":{"sec":1487521393,"usec":332977},"snapclient":{"name":"Snapclient","protocolVersion":2,"version":"0.10.0"}}],"id":"25d719fd-3a4b-2090-2ac4-b17f62b7b18b","muted":false,"name":"","stream_id":"default"}],"server":{"host":{"arch":"x86_64","ip":"","mac":"","name":"T400","os":"Linux Mint 17.3 Rosa"},"snapserver":{"controlProtocolVersion":1,"name":"Snapserver","protocolVersion":1,"version":"0.10.0"}},"streams":[{"id":"default","status":"idle","uri":{"fragment":"","host":"","path":"/tmp/snapfifo","query":{"buffer_ms":"20","codec":"flac","name":"default","sampleformat":"48000:16:2"},"raw":"pipe:///tmp/snapfifo?name=default","scheme":"pipe"}}]}}}
				vector<string> clients = request->params.get("clients");
				/// Remove clients from group
				for (auto iter = group->clients.begin(); iter != group->clients.end();)
				{
					auto client = *iter;
					if (find(clients.begin(), clients.end(), client->id) != clients.end())
					{
						++iter;
						continue;
					}
					iter = group->clients.erase(iter);
					GroupPtr newGroup = Config::instance().addClientInfo(client);
					newGroup->streamId = group->streamId;
				}

				/// Add clients to group
				PcmStreamPtr stream = streamManager_->getStream(group->streamId);
				for (const auto& clientId: clients)
				{
					ClientInfoPtr client = Config::instance().getClientInfo(clientId);
					if (!client)
						continue;
					GroupPtr oldGroup = Config::instance().getGroupFromClient(client);
					if (oldGroup && (oldGroup->id == group->id))
						continue;

					if (oldGroup)
					{
						oldGroup->removeClient(client);
						Config::instance().remove(oldGroup);
					}

					group->addClient(client);

					/// assign new stream
					session_ptr session = getStreamSession(client->id);
					if (session && stream && (session->pcmStream() != stream))
					{
						session->sendAsync(stream->getHeader());
						session->setPcmStream(stream);
					}
				}

				if (group->empty())
					Config::instance().remove(group);

				json server = Config::instance().getServerStatus(streamManager_->toJson());
				result["server"] = server;

				/// Notify others: since at least two groups are affected, send a complete server update
				notification.reset(new jsonrpcpp::Notification("Server.OnUpdate", jsonrpcpp::Parameter("server", server)));
			}
			else
				throw jsonrpcpp::MethodNotFoundException(request->id);
		}
		else if (request->method.find("Server.") == 0)
		{
			if (request->method == "Server.GetStatus")
			{
				result = Config::instance().getServerStatus(streamManager_->toJson());
			}
			else if (request->method == "Server.DeleteClient")
			{
				ClientInfoPtr clientInfo = Config::instance().getClientInfo(request->params.get("id"));
				if (clientInfo == nullptr)
					throw jsonrpcpp::InternalErrorException("Client not found", request->id);

				Config::instance().remove(clientInfo);

				json server = Config::instance().getServerStatus(streamManager_->toJson());
				result["server"] = server;

				/// Notify others
				notification.reset(new jsonrpcpp::Notification("Server.OnUpdate", jsonrpcpp::Parameter("server", server)));
			}
			else
				throw jsonrpcpp::MethodNotFoundException(request->id);
		}
		else
			throw jsonrpcpp::MethodNotFoundException(request->id);

		Config::instance().save();
		response.reset(new jsonrpcpp::Response(*request, result));
	}
	catch (const jsonrpcpp::RequestException& e)
	{
		logE << "StreamServer::onMessageReceived JsonRequestException: " << e.to_json().dump() << ", message: " << request->to_json().dump() << "\n";
		response.reset(new jsonrpcpp::RequestException(e));
	}
	catch (const exception& e)
	{
		logE << "StreamServer::onMessageReceived exception: " << e.what() << ", message: " << request->to_json().dump() << "\n";
		response.reset(new jsonrpcpp::InternalErrorException(e.what(), request->id));
	}
}


void StreamServer::onMessageReceived(ControlSession* controlSession, const std::string& message)
{
	logD << "onMessageReceived: " << message << "\n";
	jsonrpcpp::entity_ptr entity(nullptr);
	try
	{
		entity = jsonrpcpp::Parser::do_parse(message);
		if (!entity)
			return;
	}
	catch(const jsonrpcpp::ParseErrorException& e)
	{
		controlSession->send(e.to_json().dump());
		return;
	}
	catch(const std::exception& e)
	{
		controlSession->send(jsonrpcpp::ParseErrorException(e.what()).to_json().dump());
		return;
	}

	jsonrpcpp::entity_ptr response(nullptr);
	jsonrpcpp::notification_ptr notification(nullptr);
	if (entity->is_request())
	{
		jsonrpcpp::request_ptr request = dynamic_pointer_cast<jsonrpcpp::Request>(entity);
		cout << "Request:\n" << request->to_json().dump(4) << "\n";
		ProcessRequest(request, response, notification);
		if (response)
		{
			cout << "Response:\n" << response->to_json().dump(4) << "\n";
			controlSession->send(response->to_json().dump());
		}
		if (notification)
		{
			cout << "Notification:\n" << notification->to_json().dump(4) << "\n";
			controlServer_->send(notification->to_json().dump(), controlSession);
		}
	}
	else if (entity->is_batch())
	{
		jsonrpcpp::batch_ptr batch = dynamic_pointer_cast<jsonrpcpp::Batch>(entity);
		cout << "Batch: " << batch->to_json().dump() << "\n";
		jsonrpcpp::Batch responseBatch;
		jsonrpcpp::Batch notificationBatch;
		for (const auto& batch_entity: batch->entities)
		{
			if (batch_entity->is_request())
			{
				jsonrpcpp::request_ptr request = dynamic_pointer_cast<jsonrpcpp::Request>(batch_entity);
				ProcessRequest(request, response, notification);
				if (response != nullptr)
					responseBatch.add_ptr(response);
				if (notification != nullptr)
					notificationBatch.add_ptr(notification);
			}
		}
		if (!responseBatch.entities.empty())
			controlSession->send(responseBatch.to_json().dump());
		if (!notificationBatch.entities.empty())
			controlServer_->send(notificationBatch.to_json().dump(), controlSession);
	}
}



void StreamServer::onMessageReceived(StreamSession* connection, const msg::BaseMessage& baseMessage, char* buffer)
{
//	logD << "onMessageReceived: " << baseMessage.type << ", size: " << baseMessage.size << ", id: " << baseMessage.id << ", refers: " << baseMessage.refersTo << ", sent: " << baseMessage.sent.sec << "," << baseMessage.sent.usec << ", recv: " << baseMessage.received.sec << "," << baseMessage.received.usec << "\n";
	if (baseMessage.type == message_type::kTime)
	{
		msg::Time* timeMsg = new msg::Time();
		timeMsg->deserialize(baseMessage, buffer);
		timeMsg->refersTo = timeMsg->id;
		timeMsg->latency = timeMsg->received - timeMsg->sent;
//		logO << "Latency sec: " << timeMsg.latency.sec << ", usec: " << timeMsg.latency.usec << ", refers to: " << timeMsg.refersTo << "\n";
		connection->sendAsync(timeMsg, true);

		// refresh connection state
		ClientInfoPtr client = Config::instance().getClientInfo(connection->clientId);
		if (client != nullptr)
		{
			gettimeofday(&client->lastSeen, NULL);
			client->connected = true;
		}
	}
	else if (baseMessage.type == message_type::kHello)
	{
		msg::Hello helloMsg;
		helloMsg.deserialize(baseMessage, buffer);
		connection->clientId = helloMsg.getClientId();
		logO << "Hello from " << connection->clientId << ", host: " << helloMsg.getHostName() << ", v" << helloMsg.getVersion()
			<< ", ClientName: " << helloMsg.getClientName() << ", OS: " << helloMsg.getOS() << ", Arch: " << helloMsg.getArch()
			<< ", Protocol version: " << helloMsg.getProtocolVersion() << "\n";

		logD << "request kServerSettings: " << connection->clientId << "\n";
//		std::lock_guard<std::mutex> mlock(mutex_);
		bool newGroup(false);
		GroupPtr group = Config::instance().getGroupFromClient(connection->clientId);
		if (group == nullptr)
		{
			group = Config::instance().addClientInfo(connection->clientId);
			newGroup = true;
		}

		ClientInfoPtr client = group->getClient(connection->clientId);

		logD << "request kServerSettings\n";
		msg::ServerSettings* serverSettings = new msg::ServerSettings();
		serverSettings->setVolume(client->config.volume.percent);
		serverSettings->setMuted(client->config.volume.muted || group->muted);
		serverSettings->setLatency(client->config.latency);
		serverSettings->setBufferMs(settings_.bufferMs);
		serverSettings->refersTo = helloMsg.id;
		connection->sendAsync(serverSettings);

		client->host.mac = helloMsg.getMacAddress();
		client->host.ip = connection->getIP();
		client->host.name = helloMsg.getHostName();
		client->host.os = helloMsg.getOS();
		client->host.arch = helloMsg.getArch();
		client->snapclient.version = helloMsg.getVersion();
		client->snapclient.name = helloMsg.getClientName();
		client->snapclient.protocolVersion = helloMsg.getProtocolVersion();
		client->config.instance = helloMsg.getInstance();
		client->connected = true;
		gettimeofday(&client->lastSeen, NULL);

		// Assign and update stream
		PcmStreamPtr stream = streamManager_->getStream(group->streamId);
		if (!stream)
		{
			stream = streamManager_->getDefaultStream();
			group->streamId = stream->getId();
		}
		logO << "Group: " << group->id << ", stream: " << group->streamId << "\n";

		Config::instance().save();

		connection->setPcmStream(stream);
		auto headerChunk = stream->getHeader();
		connection->sendAsync(headerChunk);

		if (newGroup)
		{
			json server = Config::instance().getServerStatus(streamManager_->toJson());
			json notification = jsonrpcpp::Notification("Server.OnUpdate", jsonrpcpp::Parameter("server", server)).to_json();
			controlServer_->send(notification.dump());
			cout << "Notification:\n" << notification.dump(4) << "\n";
		}
		else
		{
			json notification = jsonrpcpp::Notification("Client.OnConnect", jsonrpcpp::Parameter("id", client->id, "client", client->toJson())).to_json();
			controlServer_->send(notification.dump());
			cout << "Notification:\n" << notification.dump(4) << "\n";
		}
//		cout << Config::instance().getServerStatus(streamManager_->toJson()).dump(4) << "\n";
//		cout << group->toJson().dump(4) << "\n";
	}
}



session_ptr StreamServer::getStreamSession(StreamSession* streamSession) const
{
	std::lock_guard<std::recursive_mutex> mlock(sessionsMutex_);
	for (auto session: sessions_)
	{
		if (session.get() == streamSession)
			return session;
	}
	return nullptr;
}


session_ptr StreamServer::getStreamSession(const std::string& clientId) const
{
//	logO << "getStreamSession: " << mac << "\n";
	std::lock_guard<std::recursive_mutex> mlock(sessionsMutex_);
	for (auto session: sessions_)
	{
		if (session->clientId == clientId)
			return session;
	}
	return nullptr;
}


void StreamServer::startAccept()
{
	socket_ptr socket = make_shared<tcp::socket>(*io_service_);
	acceptor_->async_accept(*socket, bind(&StreamServer::handleAccept, this, socket));
}


void StreamServer::handleAccept(socket_ptr socket)
{
	struct timeval tv;
	tv.tv_sec  = 5;
	tv.tv_usec = 0;
	setsockopt(socket->native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(socket->native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	/// experimental: turn on tcp::no_delay
	socket->set_option(tcp::no_delay(true));

	logS(kLogNotice) << "StreamServer::NewConnection: " << socket->remote_endpoint().address().to_string() << endl;
	shared_ptr<StreamSession> session = make_shared<StreamSession>(this, socket);

	session->setBufferMs(settings_.bufferMs);
	session->start();

	std::lock_guard<std::recursive_mutex> mlock(sessionsMutex_);
	sessions_.insert(session);

	startAccept();
}


void StreamServer::start()
{
	try
	{
		controlServer_.reset(new ControlServer(io_service_, settings_.controlPort, this));
		controlServer_->start();

		streamManager_.reset(new StreamManager(this, settings_.sampleFormat, settings_.codec, settings_.streamReadMs));
//	throw SnapException("xxx");
		for (const auto& streamUri: settings_.pcmStreams)
		{
			PcmStreamPtr stream = streamManager_->addStream(streamUri);
			if (stream)
				logO << "Stream: " << stream->getUri().toJson() << "\n";
		}
		streamManager_->start();

		acceptor_ = make_shared<tcp::acceptor>(*io_service_, tcp::endpoint(tcp::v4(), settings_.port));
		startAccept();
	}
	catch (const std::exception& e)
	{
		logS(kLogNotice) << "StreamServer::start: " << e.what() << endl;
		stop();
		throw;
	}
}


void StreamServer::stop()
{
	if (streamManager_)
	{
		streamManager_->stop();
		streamManager_ = nullptr;
	}

	{
		std::lock_guard<std::recursive_mutex> mlock(sessionsMutex_);
		for (auto session: sessions_)//it = sessions_.begin(); it != sessions_.end(); ++it)
		{
			if (session)
			{
				session->stop();
				session = nullptr;
			}
		}
		sessions_.clear();
	}

	if (controlServer_)
	{
		controlServer_->stop();
		controlServer_ = nullptr;
	}

	if (acceptor_)
	{
		acceptor_->cancel();
		acceptor_ = nullptr;
	}
}

