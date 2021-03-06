#include <proton/imperative/Sender.hpp>

#include <iostream>

using namespace proton;

Sender::Sender(work_queue* work, session& sess, const std::string& address, sender_options send_opts)
   :m_senderHandler(new SenderHandler(work))
{
   send_opts.handler(*m_senderHandler);
   // TO DO see if we can use [=] instead of [&]
   sender& sen = m_senderHandler->m_sender;
   work->add([=, &sess, &sen]() { sen = sess.open_sender(address, send_opts); });
}

Sender::Sender(Sender&& s)
   : m_senderHandler(std::move(s.m_senderHandler))
{}

Sender::~Sender()
{
   close();
}

void Sender::close()
{
   if (m_senderHandler && !m_senderHandler->m_manager.hasBeenClosedOrInError())
   {
      //we get the future before launching the action because std promise is not thread safe between get and set
      auto f = m_senderHandler->m_manager.close();
      m_senderHandler->m_work->add([=]() {m_senderHandler->m_sender.close(); });
      f.get();
   }
}

std::future<void> Sender::send(const message& mess)
{
   m_senderHandler->m_manager.checkForExceptions();
   return m_senderHandler->send(mess);
}

std::future<void> Sender::getOpenFuture()
{
   return m_senderHandler->m_manager.getOpenFuture();
}

// SenderHandler

Sender::SenderHandler::SenderHandler(work_queue* work)
   :m_work(work)
{}

Sender::SenderHandler::SenderHandler(SenderHandler&& s)
   : m_manager(std::move(s.m_manager)),
   m_sender(std::move(s.m_sender)),
   m_work(s.m_work),
   m_sentRequests(std::move(s.m_sentRequests))
{}

std::future<void> Sender::SenderHandler::send(const message& mess)
{
   //A shared_ptr to protect it from concurrent usage and correct deletion.
   //A more efficient way can be found in the future.
   std::shared_ptr<std::promise<void>> ackPromise(new std::promise<void>);
   std::future<void> ackFuture = ackPromise->get_future();
   m_work->add([=]() {
      tracker track = m_sender.send(mess);
      std::lock_guard<std::mutex> lock(m_mapMutex);
      m_sentRequests.emplace(track, ackPromise);
   });
   return ackFuture;
}

void Sender::SenderHandler::on_sender_open(sender& sen)
{
   std::cout << "client on_sender_open" << std::endl;
   m_manager.handlePnOpen();
}

void Sender::SenderHandler::on_sender_close(sender&)
{
   std::cout << "client on_sender_close" << std::endl;
   m_manager.handlePnClose(std::bind(&Sender::SenderHandler::releasePnMemberObjects, this));
}

void Sender::SenderHandler::on_sender_error(sender& sen)
{
   std::cout << "client on_sender_error" << std::endl;
   m_manager.handlePnError(sen.error().what());
   std::lock_guard<std::mutex> lock(m_mapMutex);
   for (auto it = m_sentRequests.cbegin(); it != m_sentRequests.cend(); it++)
   {
      std::shared_ptr<std::promise<void>> promise = it->second;
      promise->set_exception(std::make_exception_ptr(std::runtime_error(sen.error().what())));
   }
}

void Sender::SenderHandler::on_tracker_accept(tracker& track)
{
   std::cout << "client on_tracker_accept" << std::endl;
   auto promise = removeTrackerFromMapIfFoundElseThow(track);
   promise->set_value();
}

void Sender::SenderHandler::on_tracker_reject(tracker& track)
{
   std::cout << "client on_tracker_settle" << std::endl;
   auto promise = removeTrackerFromMapIfFoundElseThow(track);
   auto except = std::make_exception_ptr(std::runtime_error("message was rejected by peer"));
   promise->set_exception(except);
}

void Sender::SenderHandler::on_tracker_release(tracker& track)
{
   std::cout << "client on_tracker_settle" << std::endl;
   auto promise = removeTrackerFromMapIfFoundElseThow(track);
   auto except = std::make_exception_ptr(std::runtime_error("message was released by peer"));
   promise->set_exception(except);
}

std::shared_ptr<std::promise<void>> Sender::SenderHandler::removeTrackerFromMapIfFoundElseThow(tracker& track)
{
   std::lock_guard<std::mutex> lock(m_mapMutex);
   auto it = m_sentRequests.find(track);
   if (it == m_sentRequests.end())
   {
      std::string err("Illegal state tracker was not found: received ack for a message not sent");
      m_manager.handlePnError(err);
      throw std::runtime_error(err);
   }
   std::shared_ptr<std::promise<void>> promise = it->second;
   m_sentRequests.erase(it);
   return promise;
}

void Sender::SenderHandler::releasePnMemberObjects()
{
   // Reseting pn objects for thread safety
   m_sentRequests.clear();
   m_sender = sender();
}
