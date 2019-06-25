==============
HTTP Frontends
==============

.. contents::

The Ceph Object Gateway supports two embedded HTTP frontend libraries
that can be configured with ``rgw_frontends``.

Beast
=====

.. versionadded:: Luminous

The ``beast`` frontend uses the Boost.Beast library for HTTP parsing
and the Boost.Asio library for asynchronous network i/o.

Options
-------

``port``

:Description: Sets the listening port number.

:Type: Integer
:Default: ``80``


Civetweb
========

.. versionadded:: Firefly

The ``civetweb`` frontend uses the Civetweb HTTP library, which is a
fork of Mongoose.


Options
-------

``port``

:Description: Sets the listening port number. For SSL-enabled ports, add an
              ``s`` suffix like ``443s``. To bind a specific IPv4 or IPv6
              address, use the form ``address:port``. Multiple endpoints
              can be separated by ``+`` as in ``127.0.0.1:8000+443s``.

:Type: String
:Default: ``7480``


``num_threads``

:Description: Sets the number of threads spawned by Civetweb to handle
              incoming HTTP connections. This effectively limits the number
              of concurrent connections that the frontend can service.

:Type: Integer
:Default: ``rgw_thread_pool_size``


``request_timeout_ms``

:Description: The amount of time in milliseconds that Civetweb will wait
              for more incoming data before giving up.

:Type: Integer
:Default: ``30000``


``ssl_certificate``

:Description: Path to the SSL certificate file used for SSL-enabled ports.

:Type: String
:Default: None


A complete list of supported options can be found in the `Civetweb User Manual`_.


Generic Options
===============

Some frontend options are generic and supported by all frontends:

``prefix``

:Description: A prefix string that is inserted into the URI of all
              requests. For example, a swift-only frontend could supply
              a uri prefix of ``/swift``.

:Type: String
:Default: None


.. _Civetweb User Manual: https://civetweb.github.io/civetweb/UserManual.html
