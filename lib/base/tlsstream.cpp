/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012-2015 Icinga Development Team (http://www.icinga.org)    *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "base/tlsstream.hpp"
#include "base/utility.hpp"
#include "base/exception.hpp"
#include "base/logger.hpp"
#include <boost/bind.hpp>
#include <iostream>

#ifndef _WIN32
#	include <poll.h>
#endif /* _WIN32 */

using namespace icinga;

int I2_EXPORT TlsStream::m_SSLIndex;
bool I2_EXPORT TlsStream::m_SSLIndexInitialized = false;

/**
 * Constructor for the TlsStream class.
 *
 * @param role The role of the client.
 * @param sslContext The SSL context for the client.
 */
TlsStream::TlsStream(const Socket::Ptr& socket, ConnectionRole role, const boost::shared_ptr<SSL_CTX>& sslContext)
	: SocketEvents(socket, this), m_Eof(false), m_HandshakeOK(false), m_VerifyOK(true), m_ErrorCode(0),
	  m_ErrorOccurred(false),  m_Socket(socket), m_Role(role), m_SendQ(new FIFO()), m_RecvQ(new FIFO()),
	  m_CurrentAction(TlsActionNone), m_Retry(false)
{
	std::ostringstream msgbuf;
	char errbuf[120];

	m_SSL = boost::shared_ptr<SSL>(SSL_new(sslContext.get()), SSL_free);

	if (!m_SSL) {
		msgbuf << "SSL_new() failed with code " << ERR_peek_error() << ", \"" << ERR_error_string(ERR_peek_error(), errbuf) << "\"";
		Log(LogCritical, "TlsStream", msgbuf.str());

		BOOST_THROW_EXCEPTION(openssl_error()
			<< boost::errinfo_api_function("SSL_new")
			<< errinfo_openssl_error(ERR_peek_error()));
	}

	if (!m_SSLIndexInitialized) {
		m_SSLIndex = SSL_get_ex_new_index(0, const_cast<char *>("TlsStream"), NULL, NULL, NULL);
		m_SSLIndexInitialized = true;
	}

	SSL_set_ex_data(m_SSL.get(), m_SSLIndex, this);

	SSL_set_verify(m_SSL.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, &TlsStream::ValidateCertificate);

	socket->MakeNonBlocking();

	SSL_set_fd(m_SSL.get(), socket->GetFD());

	if (m_Role == RoleServer)
		SSL_set_accept_state(m_SSL.get());
	else
		SSL_set_connect_state(m_SSL.get());
}

TlsStream::~TlsStream(void)
{
	SocketEvents::Unregister();
}

int TlsStream::ValidateCertificate(int preverify_ok, X509_STORE_CTX *ctx)
{
	SSL *ssl = static_cast<SSL *>(X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx()));
	TlsStream *stream = static_cast<TlsStream *>(SSL_get_ex_data(ssl, m_SSLIndex));
	if (!preverify_ok)
		stream->m_VerifyOK = false;
	return 1;
}

bool TlsStream::IsVerifyOK(void) const
{
	return m_VerifyOK;
}

/**
 * Retrieves the X509 certficate for this client.
 *
 * @returns The X509 certificate.
 */
boost::shared_ptr<X509> TlsStream::GetClientCertificate(void) const
{
	boost::mutex::scoped_lock lock(m_Mutex);
	return boost::shared_ptr<X509>(SSL_get_certificate(m_SSL.get()), &Utility::NullDeleter);
}

/**
 * Retrieves the X509 certficate for the peer.
 *
 * @returns The X509 certificate.
 */
boost::shared_ptr<X509> TlsStream::GetPeerCertificate(void) const
{
	boost::mutex::scoped_lock lock(m_Mutex);
	return boost::shared_ptr<X509>(SSL_get_peer_certificate(m_SSL.get()), X509_free);
}

void TlsStream::OnEvent(int revents)
{
	int rc, err;
	size_t count;

	boost::mutex::scoped_lock lock(m_Mutex);

	if (!m_SSL)
		return;

	char buffer[512];

	if (m_CurrentAction == TlsActionNone) {
		if (revents & POLLIN)
			m_CurrentAction = TlsActionRead;
		else if (m_SendQ->GetAvailableBytes() > 0 && (revents & POLLOUT))
			m_CurrentAction = TlsActionWrite;
		else {
			ChangeEvents(POLLIN);
			return;
		}
	}

	switch (m_CurrentAction) {
		case TlsActionRead:
			do {
				rc = SSL_read(m_SSL.get(), buffer, sizeof(buffer));

				if (rc > 0) {
					m_RecvQ->Write(buffer, rc);
					m_CV.notify_all();
				}
			} while (SSL_pending(m_SSL.get()));

			break;
		case TlsActionWrite:
			count = m_SendQ->Peek(buffer, sizeof(buffer));

			rc = SSL_write(m_SSL.get(), buffer, count);

			if (rc > 0)
				m_SendQ->Read(NULL, rc, true);

			break;
		case TlsActionHandshake:
			rc = SSL_do_handshake(m_SSL.get());

			if (rc > 0) {
				m_HandshakeOK = true;
				m_CV.notify_all();
			}

			break;
		default:
			VERIFY(!"Invalid TlsAction");
	}

	if (rc > 0) {
		m_CurrentAction = TlsActionNone;

		if (m_SendQ->GetAvailableBytes() > 0)
			ChangeEvents(POLLIN|POLLOUT);
		else
			ChangeEvents(POLLIN);

		lock.unlock();

		if (m_RecvQ->IsDataAvailable())
			SignalDataAvailable();

		return;
	}

	err = SSL_get_error(m_SSL.get(), rc);

	std::ostringstream msgbuf;

	switch (err) {
		case SSL_ERROR_WANT_READ:
			m_Retry = true;
			ChangeEvents(POLLIN);

			break;
		case SSL_ERROR_WANT_WRITE:
			m_Retry = true;
			ChangeEvents(POLLOUT);

			break;
		case SSL_ERROR_ZERO_RETURN:
			SocketEvents::Unregister();

			m_SSL.reset();
			m_Socket->Close();
			m_Socket.reset();

			m_Eof = true;

			m_CV.notify_all();

			break;
		default:
			SocketEvents::Unregister();

			m_SSL.reset();
			m_Socket->Close();
			m_Socket.reset();

			m_ErrorCode = ERR_peek_error();
			m_ErrorOccurred = true;

			m_CV.notify_all();

			break;
	}
}

void TlsStream::HandleError(void) const
{
	if (m_ErrorOccurred) {
		BOOST_THROW_EXCEPTION(openssl_error()
		    << boost::errinfo_api_function("TlsStream::OnEvent")
		    << errinfo_openssl_error(m_ErrorCode));
	}
}

void TlsStream::Handshake(void)
{
	boost::mutex::scoped_lock lock(m_Mutex);

	m_CurrentAction = TlsActionHandshake;
	ChangeEvents(POLLOUT);

	while (!m_HandshakeOK && !m_ErrorOccurred)
		m_CV.wait(lock);

	HandleError();
}

/**
 * Processes data for the stream.
 */
size_t TlsStream::Read(void *buffer, size_t count, bool allow_partial)
{
	boost::mutex::scoped_lock lock(m_Mutex);

	if (!allow_partial)
		while (m_RecvQ->GetAvailableBytes() < count && !m_ErrorOccurred && !m_Eof)
			m_CV.wait(lock);

	HandleError();

	return m_RecvQ->Read(buffer, count, true);
}

void TlsStream::Write(const void *buffer, size_t count)
{
	boost::mutex::scoped_lock lock(m_Mutex);

	m_SendQ->Write(buffer, count);

	ChangeEvents(POLLIN|POLLOUT);
}

/**
 * Closes the stream.
 */
void TlsStream::Close(void)
{
	SocketEvents::Unregister();

	boost::mutex::scoped_lock lock(m_Mutex);

	if (!m_SSL)
		return;

	(void) SSL_shutdown(m_SSL.get());
	m_SSL.reset();

	m_Socket->Close();
	m_Socket.reset();

	m_Eof = true;
}

bool TlsStream::IsEof(void) const
{
	return m_Eof;
}

bool TlsStream::SupportsWaiting(void) const
{
	return true;
}

bool TlsStream::IsDataAvailable(void) const
{
	boost::mutex::scoped_lock lock(m_Mutex);

	return m_RecvQ->GetAvailableBytes() > 0;
}
