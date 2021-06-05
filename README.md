### **Lunule: An Agile and Judicious Metadata Load Balancer for CephFS**

#### Install Lunule

Lunule is implemented in CephFS, so if you have a CephFS cluster already, install Lunule is very convenient, you just need execute the following instructions.

```bash
git clone git@github.com:shao-xy/Lunule.git
cd Lunule/build
make -j16
sudo make install -j16
# restart mds service
sudo systemctl restart ceph-mds.target
```

If CephFS isn't exist in your cluster now, you could refer [here](#install_ceph)

#### Run CNN workload

Add the following content to */etc/ceph/ceph.conf* and restart your MDS service and start single MDS service.

```
[mds]
mds cache memory limit = 42949672960
mds reconnect timeout = 180
mds session timeout = 180

#Lunule configuration
mds bal ifenable = 1
mds bal presetmax = 8000
mds bal ifthreshold = 0.075

[client]
client cache size = 0
```

Copy your dataset into CephFS, you could find ILSVRC2012 dataset at [here](https://image-net.org/download.php) 

```
cp -r ./imagenet-dataset /mnt/your_ceph_client_path/
mkdir /mnt/your_ceph_client_path/record
```

Download MXNet tools

```
#install dependency
pip3 install scikit-build --user
pip3 install contextvars numpy mxnet opencv-python --user

#clone mxnet test tools
git clone git@github.com:apache/incubator-mxnet.git
```

 To reproduce the experiment in our paper, you could set multiple MDS 

```
ceph mds set_max_mds 5
```

and than run the following command in 100 process at the same time

```
#run CNN testcase
python3 ./incubator-mxnet/tools/im2rec.py --list --recursive /mnt/your_ceph_client_path/record /mnt/your_ceph_client_path/imagenet-dataset
```

#### <span id="install_ceph">Install CephFS on Centos from scratch</span>

Here is a small example about install CephFS on a single node.

First, update yum repository and install ceph-deploy

```
[ceph@node1 ~]$ sudo yum install -y yum-utils && sudo yum-config-manager --add-repo https://dl.fedoraproject.org/pub/epel/7/x86_64/ && sudo yum install --nogpgcheck -y epel-release && sudo rpm --import /etc/pki/rpm-gpg/RPM-GPG-KEY-EPEL-7 && sudo rm /etc/yum.repos.d/dl.fedoraproject.org*
[ceph@node1 ~]$ sudo vim /etc/yum.repos.d/ceph.repo
[ceph@node1 ~]$ sudo cat /etc/yum.repos.d/ceph.repo
[ceph-noarch]
name=Ceph noarch packages
baseurl=http://download.ceph.com/rpm-luminous/el7/noarch
enabled=1
gpgcheck=1
type=rpm-md
gpgkey=https://download.ceph.com/keys/release.asc
priority=1
[ceph@node1 ~]$ sudo yum update && sudo yum install ceph-deploy
[ceph@node1 ~]$ sudo yum install ntp ntpdate ntp-doc openssh-server
[ceph@node1 ~]$ sudo systemctl stop firewalld
[ceph@node1 ~]$ sudo iptables -A INPUT -i ib0 -p tcp -s 10.0.0.1/24 --dport 6789 -j ACCEPT
[ceph@node1 ~]$ sudo sed -i 's/SELINUX=.*/SELINUX=disabled/' /etc/selinux/config
[ceph@node1 ~]$ sudo setenforce 0
```

Create cluster and change ceph.conf

```
[ceph@node1 ~]$ mkdir ced
[ceph@node1 ~]$ cd ced
[ceph@node1 ced]$ ceph-deploy new node1
[ceph@node1 ced]$ cat ceph.conf
[global]
fsid = db2824e2-bfb7-4990-b907-8bc1f895bcd5
mon_initial_members = node1
mon_host = 10.0.0.1
auth_cluster_required = cephx
auth_service_required = cephx
auth_client_required = cephx
osd pool default size = 1
osd crush chooseleaf type = 0
public network = 10.0.0.0/24
```

Install Ceph

```
[ceph@node1 ced]$ ceph-deploy install node1 --release luminous
[ceph@node1 ced]$ ceph-deploy mon create-initial
```

Create OSD

```
[ceph@node1 ced]$ ceph-deploy osd create node1 --data /dev/your_osd_device
```

Install mgr

```
[ceph@node1 ced]$ ceph-deploy mgr create node1
```

Grant access

```
[ceph@node1 ced]$ sudo chown ceph:ceph -R /etc/ceph
[ceph@node1 ced]$ cp ceph.client.admin.keyring /etc/ceph/
[ceph@node1 ced]$ ceph -s
cluster:
  id:     0f66f8f9-a669-4aef-a222-7326d94512e8
  health: HEALTH_OK
services:
  mon: 1 daemons, quorum node16
  mgr: node1(active)
  osd: 1 osds: 1 up, 1 in
data:
  pools:   0 pools, 0 pgs
  objects: 0 objects, 0 bytes
  usage:   1024 MB used, 930 GB / 931 GB avail
  pgs:
```

Now Ceph is installed in your machine, than install CephFS

Install MDS and create your CephFS

```
[ceph@node1 ced]$[ceph@node1 ced]$ceph-deploy mds create node1
[ceph@node1 ced]$ceph osd pool create md 64 64
[ceph@node1 ced]$ ceph osd pool create md 64 64
pool 'md' created
[ceph@node1 ced]$ ceph osd pool create d 64 64
pool 'd' created
[ceph@node1 ced]$ ceph fs new myceph md d
new fs with metadata pool 1 and data pool 2
[ceph@node1 ced]$ ceph -s
cluster:
  id:     0f66f8f9-a669-4aef-a222-7326d94512e8
  health: HEALTH_OK
services:
  mon: 1 daemons, quorum node16
  mgr: node1(active)
  mds: myceph-1/1/1 up  {0=node16=up:active}
  osd: 1 osds: 1 up, 1 in
data:
  pools:   2 pools, 128 pgs
  objects: 0 objects, 0 bytes
  usage:   1025 MB used, 930 GB / 931 GB avail
  pgs:     128 active+clean
```

