/*
#######################################################################
# Copyright (C)2012, International Business Machines Corporation and
# others. All Rights Reserved.
#######################################################################
*/

#include "mcts/server.h"

#include <streams_boost/thread.hpp>
#include <streams_boost/bind.hpp>
#include <streams_boost/make_shared.hpp>
#include <streams_boost/shared_ptr.hpp>
#include <streams_boost/weak_ptr.hpp>
#include <streams_boost/lexical_cast.hpp>
#include <vector>
#include <iostream>

#include <stdlib.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <poll.h>
#include <sys/select.h>
#include <netinet/tcp.h>

namespace mcts 
{
    TCPServer::TCPServer(std::string const & address, uint32_t port, 
                         std::size_t threadPoolSize,
                         std::size_t maxConnections,
                         std::size_t maxUnreadResponseCount,
                         uint32_t blockSize,
                         outFormat_t outFormat,
                         bool broadcastResponse,
                         bool isDuplexConnection,
                         bool makeConnReadOnly,
                         DataHandler::Handler dHandler,
                         AsyncDataItem::Handler eHandler,
                         InfoHandler::Handler iHandler,
                         MetricsHandler::Handler mHandler,
                         Role roleType,
                         ConnectionSecurity secType,
                         const std::string & certificateFile, 
                         const std::string & privateKeyFile)
        : roleType_(roleType),
          securityType_(secType),
          certificateFile_(certificateFile),
          privateKeyFile_(privateKeyFile),
          threadPoolSize_(threadPoolSize),
          maxConnections_(maxConnections),
          maxUnreadResponseCount_(maxUnreadResponseCount),
          blockSize_(blockSize),
          keepAliveIdleTime_(0),
          keepAliveMaxProbesCnt_(0),
          keepAliveProbeInterval_(0),
          ioServicePool_(threadPoolSize),
          infoHandler_(iHandler),
          dataHandler_(dHandler),
          errorHandler_(eHandler),
          metricsHandler_(mHandler),
          outFormat_(outFormat),
          broadcastResponse_(broadcastResponse),
          isDuplexConnection_(isDuplexConnection),
          makeConnReadOnly_(makeConnReadOnly)
    {

      if(roleType_ == SERVER)
      {
        // if port is provided (not zero) then create a listener for it
        if (port) 
          createAcceptor(address, port);
      } 
    	
    }
    
    void TCPServer::setKeepAlive(int32_t time, int32_t probes, int32_t interval)
    {
       // Let us set the Keep Alive socket options into our member variables.
       if (time > 0)
       {
          keepAliveIdleTime_ = time;
       }

       if (probes > 0)
       {
          keepAliveMaxProbesCnt_ = probes;
       }
      
       if (interval > 0)
       {
          keepAliveProbeInterval_ = interval;
       }

       /*
       std::cout << "keepAliveIdleTime = " << keepAliveIdleTime_ << ", keepAliveMaxProbesCnt = " <<
          keepAliveMaxProbesCnt_ << ", keepAliveProbeInterval = " << keepAliveProbeInterval_ << std::endl;
       */
    }

    void TCPServer::run()
    {
        ioServicePool_.run();
    }
    
    void TCPServer::stop()
    {
        ioServicePool_.stop();
    }

    void TCPServer::handleConnect(TCPConnectionPtr conn, streams_boost::system::error_code const & e)
    {
      if(!e)
      {
        conn->start();
        mapClientConnection(conn);
        metricsHandler_.handleMetrics(0, (int64_t)mcts::TCPConnection::getNumberOfConnections());
      }
    }

    void TCPServer::handleAccept(TCPAcceptorPtr & acceptor, streams_boost::system::error_code const & e)
    {
        SPLAPPTRC(L_TRACE, "Handling Accept " << e.message(), "Server");
        if (!e) {
        	if (TCPConnection::getNumberOfConnections()<=maxConnections_) {
            SPLAPPTRC(L_TRACE, "Under Connection Limit", "Server");
        		acceptor->nextConnection()->start();

        		// Add a new connection to the response connection map, but only if duplex communication is required
        		if (isDuplexConnection_) {
              SPLAPPTRC(L_TRACE, "Connection is Duplex", "Server");
					    mapConnection(acceptor->nextConnection());
        		}
        		// Update number of open connections metric
        		metricsHandler_.handleMetrics(0, (int64_t)mcts::TCPConnection::getNumberOfConnections());


        		// Set the KeepAlive values as given by the user.
        		if (keepAliveIdleTime_ || keepAliveMaxProbesCnt_ || keepAliveProbeInterval_) {
        			int32_t _fd = static_cast<int32_t> (acceptor->nextConnection()->socket()->getNativeSocketHandle());
        			// std::cout << "Boost socket to native descriptor _fd = " << _fd << std::endl;

        			int32_t valopt = 1;
        			socklen_t len = sizeof(valopt);
        			if (setsockopt(_fd, SOL_SOCKET, SO_KEEPALIVE, &valopt, len) < 0) {
        				// std::cout << "TCPServer: SO_KEEPALIVE failed: " << std::endl;
        			}

        			if (keepAliveIdleTime_) {
        				valopt = keepAliveIdleTime_;
        				if (setsockopt(_fd, IPPROTO_TCP, TCP_KEEPIDLE, &valopt, len) < 0) {
        					// std::cout << "TCPServer: TCP_KEEPIDLE failed: " << std::endl;
        				}
        			}

        			if (keepAliveMaxProbesCnt_) {
        				valopt = keepAliveMaxProbesCnt_;
        				if (setsockopt(_fd, IPPROTO_TCP, TCP_KEEPCNT, &valopt, len) < 0) {
        					// std::cout << "TCPServer: TCP_KEEPCNT failed: " << std::endl;
        				}
        			}

        			if (keepAliveProbeInterval_) {
        				valopt = keepAliveProbeInterval_;
        				if (setsockopt(_fd, IPPROTO_TCP, TCP_KEEPINTVL, &valopt, len) < 0) {
        					// std::cout << "TCPServer: TCP_KEEPINTVL failed: " << std::endl;
        				}
        			}

        			// std::cout << "All KeepAlive values were set correctly." << std::endl;
        		}
        	}

          // The function for the socket to call back on once it's finished any operations around it's accept
          Socket::accept_complete_func func = streams_boost::bind(&TCPServer::handleAccept, this, acceptor, streams_boost::asio::placeholders::error);

        	acceptor->nextConnection().reset(new TCPConnection(securityType_, roleType_, ioServicePool_.get_io_service(), blockSize_, outFormat_, dataHandler_, infoHandler_, certificateFile_, privateKeyFile_));
          acceptor->getAcceptor().async_accept(acceptor->nextConnection()->socket()->getUnderlyingSocket(),
            streams_boost::bind(&Socket::handleAccept, acceptor->nextConnection()->socket(), func, streams_boost::asio::placeholders::error)
            );
        }
    }

    template<outFormat_t Format>
    void TCPServer::handleWrite(SPL::blob & raw, std::string const & ipAddress, uint32_t port)
    {
      SPLAPPTRC(L_TRACE, "Handling Write To " << ipAddress << ":" << port, "Server");

		  TCPConnectionWeakPtrMap::iterator iter = broadcastResponse_ ? findFirstConnection() : findConnection(createConnectionStr(ipAddress, port));
      bool serverConnFound = (iter != connMap_.end());

		  if(iter != connMap_.end())
      {
			  AsyncDataItemPtr asyncDataItemPtr = streams_boost::make_shared<AsyncDataItem>(errorHandler_);
			  asyncDataItemPtr->setData<Format>(raw);

    		do {
				TCPConnectionWeakPtr connWeakPtr = iter->second;
				TCPConnectionPtr connPtr = connWeakPtr.lock();

				/// Validate existing connection
				if (connPtr && connPtr->socket()->isOpen()) {

					uint32_t * numOutstandingWritesPtr = connPtr->getNumOutstandingWritesPtr();

					/// Check if client consumes data from a socket
					if (*numOutstandingWritesPtr <= maxUnreadResponseCount_) {
						__sync_fetch_and_add(numOutstandingWritesPtr, 1);

						if(Format == mcts::block) {
              connPtr->socket()->async_write(asyncDataItemPtr->getBuffers(), 
                streams_boost::bind(&AsyncDataItem::handleError, asyncDataItemPtr,
                              streams_boost::asio::placeholders::error, streams_boost::asio::placeholders::bytes_transferred,
                              connPtr->remoteIp(), connPtr->remotePort(), connWeakPtr)
                );

						}
						else {
							connPtr->socket()->async_write(asyncDataItemPtr->getBuffer(), 
                streams_boost::bind(&AsyncDataItem::handleError, asyncDataItemPtr,
                              streams_boost::asio::placeholders::error, streams_boost::asio::placeholders::bytes_transferred,
                              connPtr->remoteIp(), connPtr->remotePort(), connWeakPtr)
                );
						}

						iter++;
					}
					else {
						connPtr->shutdown_conn(makeConnReadOnly_);
						if(makeConnReadOnly_) {
							iter++;
						}
						else {
							iter = unmapConnection(iter);
						}

						errorHandler_.handleError(streams_boost::system::error_code(streams_boost::asio::error::would_block), 0, ipAddress, port, connWeakPtr);
					}
				}
				else {
					iter = unmapConnection(iter);
				}
			 } while (broadcastResponse_ && (iter != connMap_.end()));
      }

      TCPConnectionPtrMap::iterator clientIter = broadcastResponse_ ? findFirstClientConnection() : findClientConnection(createConnectionStr(ipAddress, port));
      bool clientConnFound = (clientIter != clientConnMap_.end());

      if(clientIter != clientConnMap_.end())
      {
        AsyncDataItemPtr asyncDataItemPtr = streams_boost::make_shared<AsyncDataItem>(errorHandler_);
        asyncDataItemPtr->setData<Format>(raw);

        do 
        {
          TCPConnectionPtr connPtr = clientIter->second;
          TCPConnectionWeakPtr connWeakPtr = TCPConnectionWeakPtr(connPtr);
          if(connPtr && connPtr->socket()->isOpen())
          {
            uint32_t * numOutstandingWritesPtr = connPtr->getNumOutstandingWritesPtr();
            /// Check if client consumes data from a socket
            if (*numOutstandingWritesPtr <= maxUnreadResponseCount_) 
            {
              __sync_fetch_and_add(numOutstandingWritesPtr, 1);

              if(Format == mcts::block) {
                connPtr->socket()->async_write(asyncDataItemPtr->getBuffers(), 
                  streams_boost::bind(&AsyncDataItem::handleError, asyncDataItemPtr,
                              streams_boost::asio::placeholders::error, streams_boost::asio::placeholders::bytes_transferred,
                              connPtr->remoteIp(), connPtr->remotePort(), connWeakPtr)
                );

              }
              else {
                connPtr->socket()->async_write(asyncDataItemPtr->getBuffer(), 
                  streams_boost::bind(&AsyncDataItem::handleError, asyncDataItemPtr,
                              streams_boost::asio::placeholders::error, streams_boost::asio::placeholders::bytes_transferred,
                              connPtr->remoteIp(), connPtr->remotePort(), connWeakPtr)
                );
              }

              clientIter++;
            }
            else
            {
              connPtr->shutdown_conn(makeConnReadOnly_);
              if(makeConnReadOnly_)
              {
                clientIter++;
              }
              else
              {
                clientIter = unmapConnection(clientIter);
              }
              errorHandler_.handleError(streams_boost::system::error_code(streams_boost::asio::error::would_block), 0, ipAddress, port, connWeakPtr);
            }
          }
        } while (broadcastResponse_ && (clientIter != clientConnMap_.end()));
      }

      if(!clientConnFound && !serverConnFound)
      {
        if(!broadcastResponse_) 
          errorHandler_.handleError(streams_boost::system::error_code(streams_boost::asio::error::connection_aborted), 0, ipAddress, port);
      }


    }

    template void TCPServer::handleWrite<line>(SPL::blob & raw, std::string const & ipAddress, uint32_t port);
    template void TCPServer::handleWrite<block>(SPL::blob & raw, std::string const & ipAddress, uint32_t port);
    template void TCPServer::handleWrite<raw>(SPL::blob & raw, std::string const & ipAddress, uint32_t port);

    void TCPServer::mapConnection(TCPConnectionPtr const & connPtr)
    {
    	std::string connStr = createConnectionStr(connPtr->remoteIp(), connPtr->remotePort());
      SPLAPPTRC(L_TRACE,"Connection String " << connStr, "Server");

		#if (((STREAMS_BOOST_VERSION / 100) % 1000) < 53)
			streams_boost::mutex::scoped_lock scoped_lock(mutex_);
		#else
			streams_boost::unique_lock<streams_boost::mutex> scoped_lock(mutex_);
		#endif

		  connMap_[connStr] = connPtr;
    }

    void TCPServer::mapClientConnection(TCPConnectionPtr const & connPtr)
    {
      std::string connStr = createConnectionStr(connPtr->remoteIp(), connPtr->remotePort());
      SPLAPPTRC(L_TRACE,"Connection String " << connStr, "Server");

    #if (((STREAMS_BOOST_VERSION / 100) % 1000) < 53)
      streams_boost::mutex::scoped_lock scoped_lock(mutex_);
    #else
      streams_boost::unique_lock<streams_boost::mutex> scoped_lock(mutex_);
    #endif

      clientConnMap_[connStr] = connPtr;
    }

    TCPConnectionWeakPtrMap::iterator TCPServer::unmapConnection(TCPConnectionWeakPtrMap::iterator iter)
    {
		#if (((STREAMS_BOOST_VERSION / 100) % 1000) < 53)
			streams_boost::mutex::scoped_lock scoped_lock(mutex_);
		#else
			streams_boost::unique_lock<streams_boost::mutex> scoped_lock(mutex_);
		#endif

		return connMap_.erase(iter);
    }

    TCPConnectionPtrMap::iterator TCPServer::unmapConnection(TCPConnectionPtrMap::iterator iter)
    {
      #if (((STREAMS_BOOST_VERSION / 100) % 1000) < 53)
        streams_boost::mutex::scoped_lock scoped_lock(mutex_);
      #else
        streams_boost::unique_lock<streams_boost::mutex> scoped_lock(mutex_);
      #endif

      return clientConnMap_.erase(iter);
    }

    TCPConnectionWeakPtrMap::iterator TCPServer::findConnection(std::string const & connStr)
    {
		#if (((STREAMS_BOOST_VERSION / 100) % 1000) < 53)
			streams_boost::mutex::scoped_lock scoped_lock(mutex_);
		#else
			streams_boost::unique_lock<streams_boost::mutex> scoped_lock(mutex_);
		#endif

		return connMap_.find(connStr);
    }

    TCPConnectionWeakPtrMap::iterator TCPServer::findFirstConnection()
    {
		#if (((STREAMS_BOOST_VERSION / 100) % 1000) < 53)
			streams_boost::mutex::scoped_lock scoped_lock(mutex_);
		#else
			streams_boost::unique_lock<streams_boost::mutex> scoped_lock(mutex_);
		#endif

		return connMap_.begin();
    }

    TCPConnectionPtrMap::iterator TCPServer::findClientConnection(std::string const & connStr)
    {
      #if (((STREAMS_BOOST_VERSION / 100) % 1000) < 53)
        streams_boost::mutex::scoped_lock scoped_lock(mutex_);
      #else
        streams_boost::unique_lock<streams_boost::mutex> scoped_lock(mutex_);
      #endif

      return clientConnMap_.find(connStr);

    }

    TCPConnectionPtrMap::iterator TCPServer::findFirstClientConnection()
    {
      #if (((STREAMS_BOOST_VERSION / 100) % 1000) < 53)
        streams_boost::mutex::scoped_lock scoped_lock(mutex_);
      #else
        streams_boost::unique_lock<streams_boost::mutex> scoped_lock(mutex_);
      #endif

      return clientConnMap_.begin();
    }

    void TCPServer::createAcceptor(std::string const & address, uint32_t port)
	  {
    	TCPAcceptorPtr acceptor(new TCPAcceptor(ioServicePool_.get_io_service(), address, port));

    	acceptor->nextConnection().reset(new TCPConnection(securityType_, roleType_, ioServicePool_.get_io_service(), blockSize_, outFormat_, dataHandler_, infoHandler_, certificateFile_, privateKeyFile_));

      Socket::accept_complete_func func = streams_boost::bind(&TCPServer::handleAccept, this, acceptor, streams_boost::asio::placeholders::error);
		  acceptor->getAcceptor().async_accept(acceptor->nextConnection()->socket()->getUnderlyingSocket(),
						      streams_boost::bind(&Socket::handleAccept, acceptor->nextConnection()->socket(), func, streams_boost::asio::placeholders::error)
                 );
	  }


    inline const std::string TCPServer::createConnectionStr(std::string const & ipAddress, uint32_t port)
    {
		return ipAddress + ":" + streams_boost::lexical_cast<std::string>(port);
    }

    void TCPServer::connect(std::string const & address, uint32_t port)
    {
      streams_boost::asio::ip::tcp::endpoint endpoint(streams_boost::asio::ip::address::from_string(address), port);
      TCPConnectionPtr conn = TCPConnectionPtr(new TCPConnection(securityType_, roleType_, ioServicePool_.get_io_service(), blockSize_, outFormat_, dataHandler_, infoHandler_, certificateFile_, privateKeyFile_));
      Socket::connect_complete_func func = streams_boost::bind(&TCPServer::handleConnect, this, conn, streams_boost::asio::placeholders::error);
      conn->socket()->getUnderlyingSocket().async_connect(endpoint, streams_boost::bind(&Socket::handleConnect, conn->socket(), func, streams_boost::asio::placeholders::error));
    }
}
