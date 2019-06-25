..  BSD LICENSE
    Copyright (C) Cavium networks Ltd. 2016.
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

    * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the
    distribution.
    * Neither the name of Cavium networks nor the names of its
    contributors may be used to endorse or promote products derived
    from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

ThunderX NICVF Poll Mode Driver
===============================

The ThunderX NICVF PMD (**librte_pmd_thunderx_nicvf**) provides poll mode driver
support for the inbuilt NIC found in the **Cavium ThunderX** SoC family
as well as their virtual functions (VF) in SR-IOV context.

More information can be found at `Cavium Networks Official Website
<http://www.cavium.com/ThunderX_ARM_Processors.html>`_.

Features
--------

Features of the ThunderX PMD are:

- Multiple queues for TX and RX
- Receive Side Scaling (RSS)
- Packet type information
- Checksum offload
- Promiscuous mode
- Multicast mode
- Port hardware statistics
- Jumbo frames
- Link state information
- Scattered and gather for TX and RX
- VLAN stripping
- SR-IOV VF
- NUMA support
- Multi queue set support (up to 96 queues (12 queue sets)) per port

Supported ThunderX SoCs
-----------------------
- CN88xx
- CN81xx
- CN83xx

Prerequisites
-------------
- Follow the DPDK :ref:`Getting Started Guide for Linux <linux_gsg>` to setup the basic DPDK environment.

Pre-Installation Configuration
------------------------------

Config File Options
~~~~~~~~~~~~~~~~~~~

The following options can be modified in the ``config`` file.
Please note that enabling debugging options may affect system performance.

- ``CONFIG_RTE_LIBRTE_THUNDERX_NICVF_PMD`` (default ``n``)

  By default it is enabled only for defconfig_arm64-thunderx-* config.
  Toggle compilation of the ``librte_pmd_thunderx_nicvf`` driver.

- ``CONFIG_RTE_LIBRTE_THUNDERX_NICVF_DEBUG_INIT`` (default ``n``)

  Toggle display of initialization related messages.

- ``CONFIG_RTE_LIBRTE_THUNDERX_NICVF_DEBUG_RX`` (default ``n``)

  Toggle display of receive fast path run-time message

- ``CONFIG_RTE_LIBRTE_THUNDERX_NICVF_DEBUG_TX`` (default ``n``)

  Toggle display of transmit fast path run-time message

- ``CONFIG_RTE_LIBRTE_THUNDERX_NICVF_DEBUG_DRIVER`` (default ``n``)

  Toggle display of generic debugging messages

- ``CONFIG_RTE_LIBRTE_THUNDERX_NICVF_DEBUG_MBOX`` (default ``n``)

  Toggle display of PF mailbox related run-time check messages

Driver Compilation
~~~~~~~~~~~~~~~~~~

To compile the ThunderX NICVF PMD for Linux arm64 gcc target, run the
following “make” command:

.. code-block:: console

   cd <DPDK-source-directory>
   make config T=arm64-thunderx-linuxapp-gcc install

Linux
-----

.. _thunderx_testpmd_example:

Running testpmd
~~~~~~~~~~~~~~~

This section demonstrates how to launch ``testpmd`` with ThunderX NIC VF device
managed by ``librte_pmd_thunderx_nicvf`` in the Linux operating system.

#. Load ``vfio-pci`` driver:

   .. code-block:: console

      modprobe vfio-pci

   .. _thunderx_vfio_noiommu:

#. Enable **VFIO-NOIOMMU** mode (optional):

   .. code-block:: console

      echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode

   .. note::

      **VFIO-NOIOMMU** is required only when running in VM context and should not be enabled otherwise.
      See also :ref:`SR-IOV: Prerequisites and sample Application Notes <thunderx_sriov_example>`.

#. Bind the ThunderX NIC VF device to ``vfio-pci`` loaded in the previous step:

   Setup VFIO permissions for regular users and then bind to ``vfio-pci``:

   .. code-block:: console

      ./tools/dpdk-devbind.py --bind vfio-pci 0002:01:00.2

#. Start ``testpmd`` with basic parameters:

   .. code-block:: console

      ./arm64-thunderx-linuxapp-gcc/app/testpmd -c 0xf -n 4 -w 0002:01:00.2 \
        -- -i --disable-hw-vlan-filter --crc-strip --no-flush-rx \
        --port-topology=loop

   Example output:

   .. code-block:: console

      ...

      PMD: rte_nicvf_pmd_init(): librte_pmd_thunderx nicvf version 1.0

      ...
      EAL:   probe driver: 177d:11 rte_nicvf_pmd
      EAL:   using IOMMU type 1 (Type 1)
      EAL:   PCI memory mapped at 0x3ffade50000
      EAL: Trying to map BAR 4 that contains the MSI-X table.
           Trying offsets: 0x40000000000:0x0000, 0x10000:0x1f0000
      EAL:   PCI memory mapped at 0x3ffadc60000
      PMD: nicvf_eth_dev_init(): nicvf: device (177d:11) 2:1:0:2
      PMD: nicvf_eth_dev_init(): node=0 vf=1 mode=tns-bypass sqs=false
           loopback_supported=true
      PMD: nicvf_eth_dev_init(): Port 0 (177d:11) mac=a6:c6:d9:17:78:01
      Interactive-mode selected
      Configuring Port 0 (socket 0)
      ...

      PMD: nicvf_dev_configure(): Configured ethdev port0 hwcap=0x0
      Port 0: A6:C6:D9:17:78:01
      Checking link statuses...
      Port 0 Link Up - speed 10000 Mbps - full-duplex
      Done
      testpmd>

.. _thunderx_sriov_example:

SR-IOV: Prerequisites and sample Application Notes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Current ThunderX NIC PF/VF kernel modules maps each physical Ethernet port
automatically to virtual function (VF) and presented them as PCIe-like SR-IOV device.
This section provides instructions to configure SR-IOV with Linux OS.

#. Verify PF devices capabilities using ``lspci``:

   .. code-block:: console

      lspci -vvv

   Example output:

   .. code-block:: console

      0002:01:00.0 Ethernet controller: Cavium Networks Device a01e (rev 01)
      ...
      Capabilities: [100 v1] Alternative Routing-ID Interpretation (ARI)
      ...
      Capabilities: [180 v1] Single Root I/O Virtualization (SR-IOV)
      ...
      Kernel driver in use: thunder-nic
      ...

   .. note::

      Unless ``thunder-nic`` driver is in use make sure your kernel config includes ``CONFIG_THUNDER_NIC_PF`` setting.

#. Verify VF devices capabilities and drivers using ``lspci``:

   .. code-block:: console

      lspci -vvv

   Example output:

   .. code-block:: console

      0002:01:00.1 Ethernet controller: Cavium Networks Device 0011 (rev 01)
      ...
      Capabilities: [100 v1] Alternative Routing-ID Interpretation (ARI)
      ...
      Kernel driver in use: thunder-nicvf
      ...

      0002:01:00.2 Ethernet controller: Cavium Networks Device 0011 (rev 01)
      ...
      Capabilities: [100 v1] Alternative Routing-ID Interpretation (ARI)
      ...
      Kernel driver in use: thunder-nicvf
      ...

   .. note::

      Unless ``thunder-nicvf`` driver is in use make sure your kernel config includes ``CONFIG_THUNDER_NIC_VF`` setting.

#. Verify PF/VF bind using ``dpdk-devbind.py``:

   .. code-block:: console

      ./tools/dpdk-devbind.py --status

   Example output:

   .. code-block:: console

      ...
      0002:01:00.0 'Device a01e' if= drv=thunder-nic unused=vfio-pci
      0002:01:00.1 'Device 0011' if=eth0 drv=thunder-nicvf unused=vfio-pci
      0002:01:00.2 'Device 0011' if=eth1 drv=thunder-nicvf unused=vfio-pci
      ...

#. Load ``vfio-pci`` driver:

   .. code-block:: console

      modprobe vfio-pci

#. Bind VF devices to ``vfio-pci`` using ``dpdk-devbind.py``:

   .. code-block:: console

      ./tools/dpdk-devbind.py --bind vfio-pci 0002:01:00.1
      ./tools/dpdk-devbind.py --bind vfio-pci 0002:01:00.2

#. Verify VF bind using ``dpdk-devbind.py``:

   .. code-block:: console

      ./tools/dpdk-devbind.py --status

   Example output:

   .. code-block:: console

      ...
      0002:01:00.1 'Device 0011' drv=vfio-pci unused=
      0002:01:00.2 'Device 0011' drv=vfio-pci unused=
      ...
      0002:01:00.0 'Device a01e' if= drv=thunder-nic unused=vfio-pci
      ...

#. Pass VF device to VM context (PCIe Passthrough):

   The VF devices may be passed through to the guest VM using qemu or
   virt-manager or virsh etc.
   ``librte_pmd_thunderx_nicvf`` or ``thunder-nicvf`` should be used to bind
   the VF devices in the guest VM in :ref:`VFIO-NOIOMMU <thunderx_vfio_noiommu>` mode.

   Example qemu guest launch command:

   .. code-block:: console

      sudo qemu-system-aarch64 -name vm1 \
      -machine virt,gic_version=3,accel=kvm,usb=off \
      -cpu host -m 4096 \
      -smp 4,sockets=1,cores=8,threads=1 \
      -nographic -nodefaults \
      -kernel <kernel image> \
      -append "root=/dev/vda console=ttyAMA0 rw hugepagesz=512M hugepages=3" \
      -device vfio-pci,host=0002:01:00.1 \
      -drive file=<rootfs.ext3>,if=none,id=disk1,format=raw  \
      -device virtio-blk-device,scsi=off,drive=disk1,id=virtio-disk1,bootindex=1 \
      -netdev tap,id=net0,ifname=tap0,script=/etc/qemu-ifup_thunder \
      -device virtio-net-device,netdev=net0 \
      -serial stdio \
      -mem-path /dev/huge

#. Refer to section :ref:`Running testpmd <thunderx_testpmd_example>` for instruction
   how to launch ``testpmd`` application.

Multiple Queue Set per DPDK port configuration
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

There are two types of VFs:

- Primary VF
- Secondary VF

Each port consists of a primary VF and n secondary VF(s). Each VF provides 8 Tx/Rx queues to a port.
When a given port is configured to use more than 8 queues, it requires one (or more) secondary VF.
Each secondary VF adds 8 additional queues to the queue set.

During PMD driver initialization, the primary VF's are enumerated by checking the
specific flag (see sqs message in DPDK boot log - sqs indicates secondary queue set).
They are at the beginning of VF list (the remain ones are secondary VF's).

The primary VFs are used as master queue sets. Secondary VFs provide
additional queue sets for primary ones. If a port is configured for more then
8 queues than it will request for additional queues from secondary VFs.

Secondary VFs cannot be shared between primary VFs.

Primary VFs are present on the beginning of the 'Network devices using kernel
driver' list, secondary VFs are on the remaining on the remaining part of the list.

   .. note::

      The VNIC driver in the multiqueue setup works differently than other drivers like `ixgbe`.
      We need to bind separately each specific queue set device with the ``tools/dpdk-devbind.py`` utility.

   .. note::

      Depending on the hardware used, the kernel driver sets a threshold ``vf_id``. VFs that try to attached with an id below or equal to
      this boundary are considered primary VFs. VFs that try to attach with an id above this boundary are considered secondary VFs.


Example device binding
~~~~~~~~~~~~~~~~~~~~~~

If a system has three interfaces, a total of 18 VF devices will be created
on a non-NUMA machine.

   .. note::

      NUMA systems have 12 VFs per port and non-NUMA 6 VFs per port.

   .. code-block:: console

      # tools/dpdk-devbind.py --status

      Network devices using DPDK-compatible driver
      ============================================
      <none>

      Network devices using kernel driver
      ===================================
      0000:01:10.0 'Device a026' if= drv=thunder-BGX unused=vfio-pci,uio_pci_generic
      0000:01:10.1 'Device a026' if= drv=thunder-BGX unused=vfio-pci,uio_pci_generic
      0002:01:00.0 'Device a01e' if= drv=thunder-nic unused=vfio-pci,uio_pci_generic
      0002:01:00.1 'Device 0011' if=eth0 drv=thunder-nicvf unused=vfio-pci,uio_pci_generic
      0002:01:00.2 'Device 0011' if=eth1 drv=thunder-nicvf unused=vfio-pci,uio_pci_generic
      0002:01:00.3 'Device 0011' if=eth2 drv=thunder-nicvf unused=vfio-pci,uio_pci_generic
      0002:01:00.4 'Device 0011' if= drv=thunder-nicvf unused=vfio-pci,uio_pci_generic
      0002:01:00.5 'Device 0011' if= drv=thunder-nicvf unused=vfio-pci,uio_pci_generic
      0002:01:00.6 'Device 0011' if= drv=thunder-nicvf unused=vfio-pci,uio_pci_generic
      0002:01:00.7 'Device 0011' if= drv=thunder-nicvf unused=vfio-pci,uio_pci_generic
      0002:01:01.0 'Device 0011' if= drv=thunder-nicvf unused=vfio-pci,uio_pci_generic
      0002:01:01.1 'Device 0011' if= drv=thunder-nicvf unused=vfio-pci,uio_pci_generic
      0002:01:01.2 'Device 0011' if= drv=thunder-nicvf unused=vfio-pci,uio_pci_generic
      0002:01:01.3 'Device 0011' if= drv=thunder-nicvf unused=vfio-pci,uio_pci_generic
      0002:01:01.4 'Device 0011' if= drv=thunder-nicvf unused=vfio-pci,uio_pci_generic
      0002:01:01.5 'Device 0011' if= drv=thunder-nicvf unused=vfio-pci,uio_pci_generic
      0002:01:01.6 'Device 0011' if= drv=thunder-nicvf unused=vfio-pci,uio_pci_generic
      0002:01:01.7 'Device 0011' if= drv=thunder-nicvf unused=vfio-pci,uio_pci_generic
      0002:01:02.0 'Device 0011' if= drv=thunder-nicvf unused=vfio-pci,uio_pci_generic
      0002:01:02.1 'Device 0011' if= drv=thunder-nicvf unused=vfio-pci,uio_pci_generic
      0002:01:02.2 'Device 0011' if= drv=thunder-nicvf unused=vfio-pci,uio_pci_generic

      Other network devices
      =====================
      0002:00:03.0 'Device a01f' unused=vfio-pci,uio_pci_generic


We want to bind two physical interfaces with 24 queues each device, we attach two primary VFs
and four secondary queues. In our example we choose two 10G interfaces eth1 (0002:01:00.2) and eth2 (0002:01:00.3).
We will choose four secondary queue sets from the ending of the list (0002:01:01.7-0002:01:02.2).


#. Bind two primary VFs to the ``vfio-pci`` driver:

   .. code-block:: console

      tools/dpdk-devbind.py -b vfio-pci 0002:01:00.2
      tools/dpdk-devbind.py -b vfio-pci 0002:01:00.3

#. Bind four primary VFs to the ``vfio-pci`` driver:

   .. code-block:: console

      tools/dpdk-devbind.py -b vfio-pci 0002:01:01.7
      tools/dpdk-devbind.py -b vfio-pci 0002:01:02.0
      tools/dpdk-devbind.py -b vfio-pci 0002:01:02.1
      tools/dpdk-devbind.py -b vfio-pci 0002:01:02.2

The nicvf thunderx driver will make use of attached secondary VFs automatically during the interface configuration stage.

Limitations
-----------

CRC striping
~~~~~~~~~~~~

The ThunderX SoC family NICs strip the CRC for every packets coming into the
host interface. So, CRC will be stripped even when the
``rxmode.hw_strip_crc`` member is set to 0 in ``struct rte_eth_conf``.

Maximum packet length
~~~~~~~~~~~~~~~~~~~~~

The ThunderX SoC family NICs support a maximum of a 9K jumbo frame. The value
is fixed and cannot be changed. So, even when the ``rxmode.max_rx_pkt_len``
member of ``struct rte_eth_conf`` is set to a value lower than 9200, frames
up to 9200 bytes can still reach the host interface.

Maximum packet segments
~~~~~~~~~~~~~~~~~~~~~~~

The ThunderX SoC family NICs support up to 12 segments per packet when working
in scatter/gather mode. So, setting MTU will result with ``EINVAL`` when the
frame size does not fit in the maximum number of segments.
