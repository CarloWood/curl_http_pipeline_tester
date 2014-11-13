// Compile this as:
//
// g++ -O2 -o http_server http_server.cpp -lboost_system
//
// Then just run ./http_server
//
// The application will listen on port 9001 and accept
// any number of connections - each connection is
// kept alive and a short text/html reply is sent
// back to the client each time the pattern "\r\n\r\n"
// is received, allowing to simulate HTTP pipelining.
//
// However, if the input contains a "X-Sleep: XXX" header then
// the server will delay sending the reply for XXX milliseconds.
// If the input contains a "X-Request: XXX" header then that
// is returned in the reply as-is. Furthermore the reply
// contains a "X-Connection:" header that enumerates the connection
// and a "X-Reply:" that enumerates the order in which replies
// were generated (which should be the same as the order in
// which the corresponding request was received obviously).

#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <iostream>
#include <string>
#include <deque>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/asio.hpp>

using boost::asio::ip::tcp;

char const* const reply =
    "HTTP/1.1 200 OK\r\n"
    "Keep-Alive: timeout=10 max=400\r\n"
    "Content-Length: %lu\r\n"
    "Content-Type: text/html\r\n"
    "X-Connection: %d\r\n"
    "X-Request: %lu\r\n"
    "X-Reply: %d\r\n"
    "\r\n"
    "<html><body>%s</body></html>\n";

char const* const reading_prefix = "    < ";
char const* const writing_prefix = "    > ";

class parser
{
  public:
    parser(std::string const& str) : m_str(str) { reset(); }
    void reset(void) { m_match = false; m_ptr = m_str.begin(); }

    operator bool(void) const { return m_match; }
    void feed(char c);

  private:
    std::string m_str;
    bool m_match;
    std::string::iterator m_ptr;
};

void parser::feed(char c)
{
  if (c != *m_ptr)
  {
    reset();
  }
  else
  {
    m_match = ++m_ptr == m_str.end();
  }
}

class header
{
  public:
    enum state_type { s_begin, s_key, s_colon, s_value, s_carriage_return, s_matched };

    header(void) { reset(); }
    void reset(void) { m_key.clear(); m_value.clear(); m_state = s_begin; m_error = false; }

    operator bool(void) const { return m_state == s_matched; }
    void feed(char c);

    std::string const& key(void) const { return m_key; }
    std::string const& value(void) const { return m_value; }

  private:
    std::string m_key;
    std::string m_value;
    state_type m_state;
    bool m_error;
};

void header::feed(char c)
{
  if (m_state == s_matched)
    reset();

  switch (m_state)
  {
    case s_key:
      if (c == ':')
      {
	m_state = s_colon;
	break;
      }
      // fall-through
    case s_begin:
      m_key += c;
      m_state = s_key;
      break;
    case s_colon:
      if (c == ' ')
	m_state = s_value;
      else
	m_error = true;
      break;
    case s_value:
      if (c == '\r')
	m_state = s_carriage_return;
      else
	m_value += c;
      break;
    case s_carriage_return:
      if (c == '\n')
      {
	if (!m_error)
	{
	  m_state = s_matched;
	  return;
	}
	else
	  reset();
      }
      else
	m_error = true;
      break;
    case s_matched:
      break;
  }
  if (c == '\n')
    reset();
}

class tcp_connection;

class Reply
{
  public:
    Reply(boost::asio::io_service& io_service, boost::shared_ptr<tcp_connection> const& connection, char const* s, size_t l) : m_timer(new boost::asio::deadline_timer(io_service)), m_str(s, l), m_sleep(0), m_connection(connection) { }
    void set_sleeping(unsigned long sleep);
    bool is_sleeping(void) const { return m_sleep != 0; }
    void wakeup(void) { if (is_sleeping()) { m_sleep = 0; m_timer.reset(); } }
    std::string const& str(void) const { return m_str; }
    void timed_out(boost::system::error_code const& error);

  private:
    boost::shared_ptr<boost::asio::deadline_timer> m_timer;
    std::string m_str;
    unsigned long m_sleep;
    boost::shared_ptr<tcp_connection> m_connection;
};

void Reply::set_sleeping(unsigned long sleep)
{
  if (sleep > 0)
  {
    m_sleep = sleep;
    m_timer->expires_from_now(boost::posix_time::millisec(m_sleep));
    m_timer->async_wait(boost::bind(&Reply::timed_out, this, boost::asio::placeholders::error));
  }
  else
  {
    wakeup();
  }
}

class tcp_connection : public boost::enable_shared_from_this<tcp_connection>
{
public:
    typedef boost::shared_ptr<tcp_connection> pointer;

    static pointer create(boost::asio::io_service& io_service, int instance)
    {
      return pointer(new tcp_connection(io_service, instance));
    }

    tcp::socket& socket()
    {
      return m_socket;
    }

    void start()
    {
      std::cout << prefix() << "Accepted a new client." << std::endl;
      m_socket.async_read_some(boost::asio::buffer(m_buffer),
          boost::bind(&tcp_connection::handle_read, shared_from_this(),
	    boost::asio::placeholders::error,
	    boost::asio::placeholders::bytes_transferred));
    }

  private:
    tcp_connection(boost::asio::io_service& io_service, int instance) :
      m_instance(instance), m_reply(0), m_closed(false), m_socket(io_service), m_eom("\r\n\r\n"), m_sleep(0), m_request(0) { }

    void handle_read(const boost::system::error_code& e, std::size_t bytes_transferred)
    {
      if (!e)
      {
	std::cout << prefix() << "Read " << bytes_transferred << " bytes:" << std::endl;

	bool new_message = true;
	for (char const* p = m_buffer.data(); p < m_buffer.data() + bytes_transferred; ++p)
	{
	  if (new_message)
	  {
	    std::cout << reading_prefix;
	    new_message = false;
	  }
	  m_eom.feed(*p);
	  m_header.feed(*p);
	  if (*p == '\n')
	  {
	    std::cout << "\\n" << std::endl;
	    if (!m_eom)
	    {
	      std::cout << reading_prefix;
	    }
	    else
	    {
	      new_message = true;
	    }
	  }
	  else if (*p == '\r')
	  {
	    std::cout << "\\r";
	  }
	  else
	  {
	    std::cout << *p;
	  }
          if (m_eom)
	  {
	    m_eom.reset();
	    m_header.reset();
	    // Send reply every time we received the sequence "\r\n\r\n".
	    queue_reply();
	    m_sleep = 0;
	  }
	  else if (m_header)
	  {
	    if (m_header.key() == "X-Sleep")
	      m_sleep = strtoul(m_header.value().data(), NULL, 10);
	    else if (m_header.key() == "X-Request")
	      m_request = strtoul(m_header.value().data(), NULL, 10);
	  }
	}

	// Always receive more data (we're pipelining).
	m_socket.async_read_some(boost::asio::buffer(m_buffer),
	    boost::bind(&tcp_connection::handle_read, shared_from_this(),
	      boost::asio::placeholders::error,
	      boost::asio::placeholders::bytes_transferred));
      }
      else if (e != boost::asio::error::operation_aborted)
      {
	std::cout << prefix() << "Error " << e << ". Closing connection." << std::endl;
	m_reply_queue.clear();
	m_socket.close();
	m_closed = true;
      }
    }

    void handle_write(const boost::system::error_code& e, size_t bytes_transferred)
    {
      if (!e)
	std::cout << prefix() << "Wrote " << bytes_transferred << " bytes." << std::endl;
      else
	std::cout << prefix() << "Error " << e << " writing data." << std::endl;
    }

    void queue_reply(void)
    {
      char body[256];
      int size = std::snprintf(body, sizeof body, "Reply %d on connection %d for request #%lu", ++m_reply, m_instance, m_request);
      assert(size < sizeof body);
      char buf[512];
      size = std::snprintf(buf, sizeof buf, reply, strlen(body) + 27, m_instance, m_request, m_reply, body);
      assert(size < sizeof buf);
      m_request = 0;
      std::string reply_formatted(buf, size);
      m_reply_queue.push_back(Reply(m_socket.get_io_service(), shared_from_this(), buf, size));
      if (m_sleep)
      {
	Reply* rp = &m_reply_queue.back();
	rp->set_sleeping(m_sleep);
      }
      process_replies();
    }

  public:
    void process_replies(void)
    {
      if (m_closed)
	return;
      if (m_reply_queue.empty())
      {
	std::cout << prefix() << "process_replies(): nothing to write." << std::endl;
	return;
      }
      while (!m_reply_queue.empty())
      {
	Reply& r = m_reply_queue.front();
	if (r.is_sleeping())
	{
	  return;
	}
	std::cout << prefix() << "process_replies(): writing data:" << std::endl;
	bool new_line = true;
	for (std::string::const_iterator p = r.str().begin(); p < r.str().end(); ++p)
	{
	  if (new_line)
	  {
	    std::cout << writing_prefix;
	    new_line = false;
	  }
	  if (*p == '\r')
	  {
	    std::cout << "\\r";
	  }
	  else if (*p == '\n')
	  {
	    std::cout << "\\n" << std::endl;
	    new_line = true;
	  }
	  else
	  {
	    std::cout << *p;
	  }
	}
	boost::asio::async_write(m_socket, boost::asio::buffer(r.str()),
	    boost::bind(&tcp_connection::handle_write, shared_from_this(),
	      boost::asio::placeholders::error,
	      boost::asio::placeholders::bytes_transferred));
	m_reply_queue.pop_front();
      }
    }

    std::string prefix(void) const
    {
      struct timeval tv;
      time_t nowtime;
      struct tm *nowtm;
      char tmbuf[64], buf[80];

      gettimeofday(&tv, NULL);
      nowtime = tv.tv_sec;
      nowtm = localtime(&nowtime);
      strftime(tmbuf, sizeof tmbuf, "%Y-%m-%d %H:%M:%S", nowtm);
      snprintf(buf, sizeof buf, "%s.%06lu: #%d: ", tmbuf, tv.tv_usec, m_instance);
      return buf;
    }

  private:
    int m_instance;
    int m_reply;
    bool m_closed;
    tcp::socket m_socket;
    boost::array<char, 8192> m_buffer;
    parser m_eom;
    header m_header;
    unsigned long m_sleep;
    unsigned long m_request;
    std::deque<Reply> m_reply_queue;
};

void Reply::timed_out(boost::system::error_code const& error)
{
  if (!error)
  {
    m_sleep = 0;		// Not sleeping anymore.
    m_connection->process_replies();
  }
}

class tcp_server
{
  public:
    tcp_server(boost::asio::io_service& io_service) : m_acceptor(io_service, tcp::endpoint(tcp::v4(), 9001)), m_count(0)
    {
      std::cout << "Listening on port 9001..." << std::endl;
      m_acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
      start_accept();
    }

  private:
    void start_accept()
    {
      tcp_connection::pointer new_connection = tcp_connection::create(m_acceptor.get_io_service(), ++m_count);
      m_acceptor.async_accept(new_connection->socket(), boost::bind(&tcp_server::handle_accept, this, new_connection, boost::asio::placeholders::error));
    }

    void handle_accept(tcp_connection::pointer new_connection, boost::system::error_code const& error)
    {
      if (!error)
      {
	new_connection->start();
      }
      start_accept();
    }

    tcp::acceptor m_acceptor;
    int m_count;
};

int main()
{
  try
  {
    boost::asio::io_service io_service;
    tcp_server server(io_service);
    io_service.run();
  }
  catch (std::exception& e)
  {
    std::cerr << e.what() << std::endl;
  }

  return 0;
}

