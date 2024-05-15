pagesize_kb=$(grep Hugepagesize /proc/meminfo | awk '{print $2}')
required_kb=$((2 * 1024 * 1024))
pages=$((required_kb / pagesize_kb))
echo "Required pages: $pages"

sudo sysctl -w vm.nr_hugepages=$pages
sudo ./scripts/setup.sh
sudo HUGEMEM=2048 DRIVER_OVERRIDE=uio_pci_generic ./scripts/setup.sh
