/**
 ******************************************************************************
  Copyright (c) 2013-2015 Particle Industries, Inc.  All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation, either
  version 3 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************
 */

#include "dtls_message_channel.h"
#include "service_debug.h"
#include "rng_hal.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl_internal.h"
#include "timer_hal.h"
#include <stdio.h>
#include <string.h>

namespace particle { namespace protocol {


class SessionPersist
{
public:

	using save_fn_t = decltype(DTLSMessageChannel::Callbacks::save);
	using restore_fn_t = decltype(DTLSMessageChannel::Callbacks::restore);

private:

	uint16_t size;

	uint8_t randbytes[sizeof(mbedtls_ssl_handshake_params::randbytes)];
	decltype(mbedtls_ssl_session::ciphersuite) ciphersuite;
	decltype(mbedtls_ssl_session::compression) compression;
	decltype(mbedtls_ssl_session::id_len) id_len;
	uint8_t id[sizeof(mbedtls_ssl_session::id)];
	uint8_t master[sizeof(mbedtls_ssl_session::master)];
	decltype(mbedtls_ssl_context::in_epoch) in_epoch;
	unsigned char out_ctr[8];

	void restore_session(mbedtls_ssl_session* session)
	{
		session->ciphersuite = ciphersuite;
		session->compression = compression;
		session->id_len = id_len;
		memcpy(session->id, id, sizeof(id));
		memcpy(session->master, master, sizeof(master));
	}

	void save_session(mbedtls_ssl_session* session)
	{
		ciphersuite = session->ciphersuite;
		compression = session->compression;
		id_len = session->id_len;
		memcpy(id, session->id, sizeof(id));
		memcpy(master, session->master, sizeof(master));
	}

	bool restore_this_from(restore_fn_t restorer)
	{
		if (restorer)
		{
			if (restorer(this, sizeof(*this))!=sizeof(*this))
				return false;
		}
		return true;
	}

	void save_this_with(save_fn_t saver)
	{
		if (saver)
			saver(this, sizeof(*this));
	}

public:

	void clear(save_fn_t saver)
	{
		size = 0;
		save_this_with(saver);
	}

	void save(const uint8_t* random, mbedtls_ssl_context* context, save_fn_t saver);
	void update(mbedtls_ssl_context* context, save_fn_t saver);

	bool restore(mbedtls_ssl_context* context, restore_fn_t restorer);

};

void SessionPersist::save(const uint8_t* random, mbedtls_ssl_context* context, save_fn_t saver)
{
	if (context->state == MBEDTLS_SSL_HANDSHAKE_OVER)
	{
		in_epoch = context->in_epoch;
		memcpy(out_ctr, context->out_ctr, 8);
		memcpy(randbytes, random, sizeof(randbytes));
		save_session(context->session);
		save_this_with(saver);
		size = sizeof(*this);
	}
	else
	{
		// invalid state
		clear(saver);
	}
}

void SessionPersist::update(mbedtls_ssl_context* context, save_fn_t saver)
{
	if (context->state == MBEDTLS_SSL_HANDSHAKE_OVER)
	{
		memcpy(out_ctr, context->out_ctr, 8);
		save_this_with(saver);
		size = sizeof(*this);
	}
}

bool SessionPersist::restore(mbedtls_ssl_context* context, restore_fn_t restorer)
{
	if (!restore_this_from(restorer))
		return false;

	if (size!=sizeof(*this))
		return false;

	mbedtls_ssl_session_reset(context);

	context->state = MBEDTLS_SSL_HANDSHAKE_WRAPUP;
	context->handshake->resume = 1;
	context->in_epoch = in_epoch;
	memcpy(context->out_ctr, &out_ctr, 8);
	memcpy(context->handshake->randbytes, randbytes, sizeof(randbytes));
	restore_session(context->session_negotiate);
	context->transform_negotiate->ciphersuite_info = mbedtls_ssl_ciphersuite_from_id(ciphersuite);

	if (!context->transform_negotiate->ciphersuite_info)
	{
		DEBUG("unknown ciphersuite with id %d", ciphersuite);
		return false;
	}

	int err = mbedtls_ssl_derive_keys(context);
	if (err)
	{
		DEBUG("derive keys failed with %d", err);
		return false;
	}

	context->in_msg = context->in_iv + context->transform_negotiate->ivlen -
										context->transform_negotiate->fixed_ivlen;
	context->out_msg = context->out_iv + context->transform_negotiate->ivlen -
										 context->transform_negotiate->fixed_ivlen;

	context->session_in = context->session_negotiate;
	context->session_out = context->session_negotiate;
	
	context->transform_in = context->transform_negotiate;
	context->transform_out = context->transform_negotiate;
	
	mbedtls_ssl_handshake_wrapup(context);

	return true;
}


SessionPersist sessionPersist;

// mbedtls_ecp_gen_keypair
// see also gen_key.c and mbedtls_ecp_gen_key

#define EXIT_ERROR(x, msg) \
	if (x) { \
		WARN("DTLS initialization failure: " #msg ": %c%04X",(x<0)?'-':' ',(x<0)?-x:x);\
		return UNKNOWN; \
	}

static void my_debug( void *ctx, int level,
		const char *file, int line,
		const char *str )
{
#if PLATFORM_ID!=3
	DEBUG_D("%s:%04d: %s", file, line, str);
#else
	fprintf(stdout, "%s:%04d: %s", file, line, str);
	fflush(stdout);
#endif
}

// todo - would like to make this a callback
int dtls_rng(void* handle, uint8_t* data, const size_t len_)
{
	size_t len = len_;
	while (len>=4)
	{
		*((uint32_t*)data) = HAL_RNG_GetRandomNumber();
		data += 4;
		len -= 4;
	}
	while (len-->0)
	{
		*data++ = HAL_RNG_GetRandomNumber();
	}

	return 0;
}

ProtocolError DTLSMessageChannel::init(
		const uint8_t* core_private, size_t core_private_len,
		const uint8_t* core_public, size_t core_public_len,
		const uint8_t* server_public, size_t server_public_len,
		const uint8_t* device_id, Callbacks& callbacks)
{
	init();
	int ret;
	this->callbacks = callbacks;

	ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
			MBEDTLS_SSL_TRANSPORT_DATAGRAM, MBEDTLS_SSL_PRESET_DEFAULT);
	EXIT_ERROR(ret, "unable to configure defaults");

	mbedtls_ssl_conf_handshake_timeout(&conf, 3000, 6000);

	mbedtls_ssl_conf_rng(&conf, dtls_rng, this);
	mbedtls_ssl_conf_dbg(&conf, my_debug, nullptr);
	mbedtls_ssl_conf_min_version(&conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);

	ret = mbedtls_pk_parse_public_key(&pkey, core_public, core_public_len);
	EXIT_ERROR(ret, "unable to parse device public key");

	ret = mbedtls_pk_parse_key(&pkey, core_private, core_private_len, NULL, 0);
	EXIT_ERROR(ret, "unable to parse device private key");

	ret = mbedtls_ssl_conf_own_cert(&conf, &clicert, &pkey);
	EXIT_ERROR(ret, "unable to configure own certificate");

	mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
	static int ssl_cert_types[] = { MBEDTLS_TLS_CERT_TYPE_RAW_PUBLIC_KEY, MBEDTLS_TLS_CERT_TYPE_NONE };
	mbedtls_ssl_conf_client_certificate_types(&conf, ssl_cert_types);
	mbedtls_ssl_conf_server_certificate_types(&conf, ssl_cert_types);
	mbedtls_ssl_conf_certificate_receive(&conf, MBEDTLS_SSL_RECEIVE_CERTIFICATE_DISABLED);

	this->server_public = new uint8_t[server_public_len];
	memcpy(this->server_public, server_public, server_public_len);
	this->server_public_len = server_public_len;
	return NO_ERROR;
}

inline int DTLSMessageChannel::send(const uint8_t* data, size_t len)
{
	return callbacks.send(data, len, callbacks.tx_context);
}

inline int DTLSMessageChannel::recv(uint8_t* data, size_t len)
{
	return callbacks.receive(data, len, callbacks.tx_context);
}

int DTLSMessageChannel::send_( void *ctx, const unsigned char *buf, size_t len ) {
	DTLSMessageChannel* channel = (DTLSMessageChannel*)ctx;
	int count = channel->send(buf, len);
	if (count == 0)
		return MBEDTLS_ERR_SSL_WANT_WRITE;

	return count;
}

int DTLSMessageChannel::recv_( void *ctx, unsigned char *buf, size_t len ) {
	DTLSMessageChannel* channel = (DTLSMessageChannel*)ctx;
	int count = channel->recv(buf, len);
	if (count == 0) {
		// 0 means EOF in this context
		return MBEDTLS_ERR_SSL_WANT_READ;
	}
	return count;
}


void DTLSMessageChannel::init()
{
	server_public = nullptr;
	mbedtls_ssl_init (&ssl_context);
	mbedtls_ssl_config_init (&conf);
	mbedtls_x509_crt_init (&clicert);
	mbedtls_pk_init (&pkey);
	mbedtls_debug_set_threshold(1);
}

void DTLSMessageChannel::dispose()
{
	mbedtls_x509_crt_free (&clicert);
	mbedtls_pk_free (&pkey);
	mbedtls_ssl_config_free (&conf);
	mbedtls_ssl_free (&ssl_context);
	delete this->server_public;
	server_public_len = 0;
}


ProtocolError DTLSMessageChannel::setup_context()
{
	int ret;
	ret = mbedtls_ssl_setup(&ssl_context, &conf);
	EXIT_ERROR(ret, "unable to setup SSL context");

	mbedtls_ssl_set_timer_cb(&ssl_context, &timer, mbedtls_timing_set_delay, mbedtls_timing_get_delay);
	mbedtls_ssl_set_bio(&ssl_context, this, &DTLSMessageChannel::send_, &DTLSMessageChannel::recv_, NULL);

	if ((ssl_context.session_negotiate->peer_cert = (mbedtls_x509_crt*)calloc(1, sizeof(mbedtls_x509_crt))) == NULL)
	{
		ERROR("unable to allocate certificate storage");
		return INSUFFICIENT_STORAGE;
	}

	mbedtls_x509_crt_init(ssl_context.session_negotiate->peer_cert);
	ret = mbedtls_pk_parse_public_key(&ssl_context.session_negotiate->peer_cert->pk, server_public, server_public_len);
	if (ret) {
		WARN("unable to parse negotiated public key: -%x", -ret);
		return IO_ERROR;
	}

	return NO_ERROR;
}

ProtocolError DTLSMessageChannel::establish()
{
	int ret = 0;

	ProtocolError error = setup_context();
	if (error)
		return error;

	if (sessionPersist.restore(&ssl_context, callbacks.restore))
	{
		DEBUG("restored session from persisted session data");
		return SESSION_RESUMED;
	}

	uint8_t random[64];

	do
	{
		while (ssl_context.state != MBEDTLS_SSL_HANDSHAKE_OVER)
		{
			ret = mbedtls_ssl_handshake_step(&ssl_context);

			if (ret != 0)
				break;

			// we've already received the ServerHello, thus
			// we have the random values for client and server
			if (ssl_context.state == MBEDTLS_SSL_SERVER_KEY_EXCHANGE)
			{
				memcpy(random, ssl_context.handshake->randbytes, 64);
			}
		}
	}
	while(ret == MBEDTLS_ERR_SSL_WANT_READ ||
	      ret == MBEDTLS_ERR_SSL_WANT_WRITE);

	if (ret)
	{
		mbedtls_ssl_session_reset(&ssl_context);
		DEBUG("handshake failed -%x", -ret);
		sessionPersist.clear(callbacks.save);
	}
	else
	{
		sessionPersist.save(random, &ssl_context, callbacks.save);
	}
	return ret==0 ? NO_ERROR : IO_ERROR;
}

ProtocolError DTLSMessageChannel::receive(Message& message)
{
	if (ssl_context.state != MBEDTLS_SSL_HANDSHAKE_OVER)
		return INVALID_STATE;

	create(message);
	uint8_t* buf = message.buf();
	size_t len = message.capacity();

	conf.read_timeout = 0;
	int ret = mbedtls_ssl_read(&ssl_context, buf, len);
	if (ret<=0) {
		switch (ret) {
		case MBEDTLS_ERR_SSL_WANT_READ:
			break;
		case MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE:
			ret = 0;
			break;
		default:
			mbedtls_ssl_session_reset(&ssl_context);
			return IO_ERROR;

		}
	}
	message.set_length(ret);
	return NO_ERROR;
}

ProtocolError DTLSMessageChannel::send(Message& message)
{
  if (ssl_context.state != MBEDTLS_SSL_HANDSHAKE_OVER)
    return INVALID_STATE;

  if (message.length()>20)
      DEBUG("message length %d, last 20 bytes %s ", message.length(), message.buf()+message.length()-20);
  else
      DEBUG("message length %d ", message.length());

  int ret = mbedtls_ssl_write(&ssl_context, message.buf(), message.length());
  if (ret < 0 && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
  {
		mbedtls_ssl_session_reset(&ssl_context);
		return IO_ERROR;
  }
  sessionPersist.update(&ssl_context, callbacks.save);
  return NO_ERROR;
}

bool DTLSMessageChannel::is_unreliable()
{
	return true;
}


}}

#include "sys/time.h"

extern "C" unsigned long mbedtls_timing_hardclock()
{
	return HAL_Timer_Microseconds();
}

extern "C" int _gettimeofday( struct timeval *tv, void *tzvp )
{
    uint32_t t = HAL_Timer_Milliseconds();  // get uptime in nanoseconds
    tv->tv_sec = t / 1000;  // convert to seconds
    tv->tv_usec = ( t % 1000 )*1000;  // get remaining microseconds
    return 0;  // return non-zero for error
} // end _gettimeofday()


