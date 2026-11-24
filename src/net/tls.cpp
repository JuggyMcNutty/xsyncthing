#include "net/tls.hpp"

#include <mutex>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/error-crypt.h>

namespace xst {

namespace {

std::string ssl_error(WOLFSSL *ssl, int ret)
{
	const int err = wolfSSL_get_error(ssl, ret);
	char text[WOLFSSL_MAX_ERROR_SZ] = {};
	wolfSSL_ERR_error_string_n(static_cast<unsigned long>(err), text, sizeof text);
	return std::to_string(err) + " " + text;
}

int io_recv(WOLFSSL *, char *buffer, int size, void *context)
{
	auto *stream = static_cast<Stream *>(context);
	const long n = stream->read(std::span(reinterpret_cast<uint8_t *>(buffer), static_cast<size_t>(size)));
	if (n == 0)
		return WOLFSSL_CBIO_ERR_CONN_CLOSE;
	if (n < 0)
		return WOLFSSL_CBIO_ERR_GENERAL;
	return static_cast<int>(n);
}

int io_send(WOLFSSL *, char *buffer, int size, void *context)
{
	auto *stream = static_cast<Stream *>(context);
	const long n = stream->write(std::span(reinterpret_cast<const uint8_t *>(buffer), static_cast<size_t>(size)));
	if (n <= 0)
		return WOLFSSL_CBIO_ERR_CONN_RST;
	return static_cast<int>(n);
}

// Peers are self-signed: accept any chain here, and let the caller check the device ID (as Syncthing does with
// InsecureSkipVerify plus its own check of the certificate's hash).
int accept_any(int, WOLFSSL_X509_STORE_CTX *) { return 1; }

} // namespace

struct TlsConnection::Impl {
	std::unique_ptr<Stream> stream;
	WOLFSSL_CTX *ctx = nullptr;
	WOLFSSL *ssl = nullptr;
	~Impl()
	{
		if (ssl)
			wolfSSL_free(ssl);
		if (ctx)
			wolfSSL_CTX_free(ctx);
	}
};

void tls_init()
{
	static std::once_flag once;
	std::call_once(once, [] { wolfSSL_Init(); });
}

TlsConnection::~TlsConnection() { close(); }

std::unique_ptr<TlsConnection> TlsConnection::handshake(std::unique_ptr<Stream> stream, const TlsOptions &options,
	std::string *error)
{
	tls_init();
	auto fail = [&](const std::string &what) {
		if (error)
			*error = what;
		return nullptr;
	};

	std::unique_ptr<TlsConnection> conn(new TlsConnection());
	conn->impl_ = std::make_unique<Impl>();
	Impl &impl = *conn->impl_;
	impl.stream = std::move(stream);

	const bool server = options.role == TlsOptions::Role::server;
	impl.ctx = wolfSSL_CTX_new(server ? wolfSSLv23_server_method() : wolfSSLv23_client_method());
	if (!impl.ctx)
		return fail("wolfSSL_CTX_new failed");
	if (wolfSSL_CTX_SetMinVersion(impl.ctx, options.min_version >= 13 ? WOLFSSL_TLSV1_3 : WOLFSSL_TLSV1_2) !=
		WOLFSSL_SUCCESS)
		return fail("wolfSSL_CTX_SetMinVersion failed");

	if (options.identity) {
		const auto &id = *options.identity;
		if (wolfSSL_CTX_use_certificate_buffer(impl.ctx, id.cert_der.data(), static_cast<long>(id.cert_der.size()),
				WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS)
			return fail("loading our certificate failed");
		if (wolfSSL_CTX_use_PrivateKey_buffer(impl.ctx, id.key_der.data(), static_cast<long>(id.key_der.size()),
				WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS)
			return fail("loading our private key failed");
	}

	if (!options.ca_pem.empty()) {
		if (wolfSSL_CTX_load_verify_buffer(impl.ctx, reinterpret_cast<const unsigned char *>(options.ca_pem.data()),
				static_cast<long>(options.ca_pem.size()), WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS)
			return fail("loading CA roots failed");
		wolfSSL_CTX_set_verify(impl.ctx, WOLFSSL_VERIFY_PEER, nullptr);
	} else {
		int mode = WOLFSSL_VERIFY_PEER;
		if (options.require_peer_cert && server)
			mode |= WOLFSSL_VERIFY_FAIL_IF_NO_PEER_CERT;
		if (!options.require_peer_cert && server)
			mode = WOLFSSL_VERIFY_NONE;
		wolfSSL_CTX_set_verify(impl.ctx, mode, accept_any);
	}

	wolfSSL_CTX_SetIORecv(impl.ctx, io_recv);
	wolfSSL_CTX_SetIOSend(impl.ctx, io_send);

	impl.ssl = wolfSSL_new(impl.ctx);
	if (!impl.ssl)
		return fail("wolfSSL_new failed");
	wolfSSL_SetIOReadCtx(impl.ssl, impl.stream.get());
	wolfSSL_SetIOWriteCtx(impl.ssl, impl.stream.get());

	if (!options.alpn.empty()) {
		std::string list;
		for (const auto &p : options.alpn) {
			if (!list.empty())
				list.push_back(',');
			list += p;
		}
		if (wolfSSL_UseALPN(impl.ssl, list.data(), static_cast<unsigned>(list.size()),
				WOLFSSL_ALPN_FAILED_ON_MISMATCH) != WOLFSSL_SUCCESS)
			return fail("wolfSSL_UseALPN failed");
	}
	if (!options.server_name.empty() && !server) {
		wolfSSL_UseSNI(impl.ssl, WOLFSSL_SNI_HOST_NAME, options.server_name.data(),
			static_cast<unsigned short>(options.server_name.size()));
		if (!options.ca_pem.empty())
			wolfSSL_check_domain_name(impl.ssl, options.server_name.c_str());
	}

	const int ret = server ? wolfSSL_accept(impl.ssl) : wolfSSL_connect(impl.ssl);
	if (ret != WOLFSSL_SUCCESS)
		return fail("TLS handshake failed: " + ssl_error(impl.ssl, ret));

	if (WOLFSSL_X509_CHAIN *chain = wolfSSL_get_peer_chain(impl.ssl); chain && wolfSSL_get_chain_count(chain) > 0) {
		const unsigned char *der = wolfSSL_get_chain_cert(chain, 0);
		const int length = wolfSSL_get_chain_length(chain, 0);
		if (der && length > 0)
			conn->peer_cert_.assign(der, der + length);
	}
	if (options.require_peer_cert && conn->peer_cert_.empty())
		return fail("the peer sent no certificate");

	char *protocol = nullptr;
	unsigned short protocol_size = 0;
	if (wolfSSL_ALPN_GetProtocol(impl.ssl, &protocol, &protocol_size) == WOLFSSL_SUCCESS && protocol)
		conn->alpn_.assign(protocol, protocol_size);
	return conn;
}

long TlsConnection::read(std::span<uint8_t> buffer)
{
	if (!impl_ || !impl_->ssl)
		return -1;
	const int n = wolfSSL_read(impl_->ssl, buffer.data(), static_cast<int>(buffer.size()));
	if (n > 0)
		return n;
	const int err = wolfSSL_get_error(impl_->ssl, n);
	if (err == WOLFSSL_ERROR_ZERO_RETURN || err == SOCKET_PEER_CLOSED_E)
		return 0;
	return -1;
}

bool TlsConnection::read_full(std::span<uint8_t> buffer)
{
	size_t done = 0;
	while (done < buffer.size()) {
		const long n = read(buffer.subspan(done));
		if (n <= 0)
			return false;
		done += static_cast<size_t>(n);
	}
	return true;
}

bool TlsConnection::write_all(std::span<const uint8_t> data)
{
	if (!impl_ || !impl_->ssl)
		return false;
	size_t done = 0;
	while (done < data.size()) {
		const int n = wolfSSL_write(impl_->ssl, data.data() + done, static_cast<int>(data.size() - done));
		if (n <= 0)
			return false;
		done += static_cast<size_t>(n);
	}
	return true;
}

void TlsConnection::close()
{
	if (!impl_)
		return;
	if (impl_->ssl)
		wolfSSL_shutdown(impl_->ssl);
	if (impl_->stream)
		impl_->stream->close();
	impl_.reset();
}

std::string TlsConnection::version() const
{
	return impl_ && impl_->ssl ? wolfSSL_get_version(impl_->ssl) : "";
}

std::string TlsConnection::cipher() const
{
	return impl_ && impl_->ssl ? wolfSSL_get_cipher(impl_->ssl) : "";
}

} // namespace xst
