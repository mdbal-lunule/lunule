### **Lunule: An Agile and Judicious Metadata Load Balancer for CephFS**

#### Install Lunule

Lunule is implemented atop CephFS and easy to replace the existing CephFS's metadata service. Run the following commands in MDS to install Lunule .

```bash
git clone git@github.com:shao-xy/Lunule.git
cd Lunule/build
make -j16
sudo make install -j16
# restart mds service
sudo systemctl restart ceph-mds.target
```

If CephFS doesn't exist in your cluster, refer [here](#install_ceph)

#### Run CNN workload

Add the following content to */etc/ceph/ceph.conf* and restart with a single MDS service.

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

Copy [ILSVRC2012](https://image-net.org/download.php)  dataset into cluster.

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

 Activate multiple MDS

```
ceph mds set_max_mds 5
```

and then run the following commands 100 times concurrently

```
#run CNN testcase
python3 ./incubator-mxnet/tools/im2rec.py --list --recursive /mnt/your_ceph_client_path/record /mnt/your_ceph_client_path/imagenet-dataset
```

#### <span id="install_ceph">Install CephFS on CentOS from scratch</span>

Here is a small example about install CephFS on a single node.

First, update YUM repository and install `ceph-deploy`.

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

Then create a cluster and modify `ceph.conf`

```
[ceph@node1 ~]$ mkdir ced
[ceph@node1 ~]$ cd ced
[ceph@node1 ced]$ ceph-deploy new node1
[ceph@node1 ced]$ cat << EOF > ceph.conf
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

EOF
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
[ceph@node1 ced]$ ceph-deploy admin node1
[ceph@node1 ced]$ ceph -s
cluster:
  id:     0f66f8f9-a669-4aef-a222-7326d94512e8
  health: HEALTH_OK
services:
  mon: 1 daemons, quorum node1
  mgr: node1(active)
  osd: 1 osds: 1 up, 1 in
data:
  pools:   0 pools, 0 pgs
  objects: 0 objects, 0 bytes
  usage:   1024 MB used, 930 GB / 931 GB avail
  pgs:
```

Now Ceph is installed  successfully, then install CephFS

Install MDS and create your CephFS

```
[ceph@node1 ced]$ ceph-deploy mds create node1
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
  mon: 1 daemons, quorum node1
  mgr: node1(active)
  mds: myceph-1/1/1 up  {0=node1=up:active}
  osd: 1 osds: 1 up, 1 in
data:
  pools:   2 pools, 128 pgs
  objects: 0 objects, 0 bytes
  usage:   1025 MB used, 930 GB / 931 GB avail
  pgs:     128 active+clean
```

