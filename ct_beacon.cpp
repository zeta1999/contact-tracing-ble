#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <poll.h>

#include <iostream>
#include <sstream>
#include <unistd.h>

#include "ct_beacon.h"

// Hi. Guess who learned a lot about Bluetooth Low Energy advertising today?

const uint8_t FLAGS_TYPE = 0x01;
const uint8_t SERVICE_UUID16_TYPE = 0x03;
const uint8_t SERVICE_DATA16_TYPE = 0x16;

const uint8_t CT_FLAGS = 0x1A;
const uint16_t CT_SERVICE_UUID16 = 0xFD6F;

uint8_t build_ct_packet(uint8_t* packet_data, const std::vector<uint8_t>& rpi) {
    // Flags section
    packet_data[0] = 0x02; // section length
    packet_data[1] = FLAGS_TYPE;
    packet_data[2] = CT_FLAGS;
    // UUID16 section
    packet_data[3] = 0x03; // section length
    packet_data[4] = SERVICE_UUID16_TYPE;
    *(uint16_t*)(packet_data+5) = htobs(CT_SERVICE_UUID16);
    // Data section
    packet_data[7] = 0x13;
    packet_data[8] = SERVICE_DATA16_TYPE;
    *(uint16_t*)(packet_data+9) = htobs(CT_SERVICE_UUID16);
    for (auto i = 0; i < 16; i++) packet_data[11+i] = rpi[i];
    return 27;
}

// recommended advertising interval -- ~200-270 ms
const auto MIN_INTERVAL_MS = 200;
const auto MAX_INTERVAL_MS = 270;
const auto MS_PER_INTERVAL = 0.625;

CT_Beacon::CT_Beacon(const std::string& device_name) {
    int dev_id = hci_devid(device_name.c_str());
    if (dev_id < 0) throw std::runtime_error("failed hci_devid");
    dev = hci_open_dev(dev_id);
    if (dev < 0) throw std::runtime_error("failed to open hci device");
}

CT_Beacon::~CT_Beacon() {
    if (dev >= 0) hci_close_dev(dev);
}

void CT_Beacon::do_req(struct hci_request& rq) {
    uint8_t status = 0;
    rq.rparam = &status;
    rq.rlen = 1;
    int ret = hci_send_req(dev, &rq, 1000);
    if (ret < 0) {
        throw std::runtime_error("Could not send HCI request");
    } else if (status != 0) {
        std::stringstream s;
        s << "HCI error during " << std::hex << (int)rq.ocf << ": " << (int)status;
        throw std::runtime_error(s.str());
    }
}

void CT_Beacon::reset() {}


void CT_Beacon::start_advertising(const std::vector<uint8_t>& rpi) {
    //
    // Set advertising parameters
    //
    le_set_advertising_parameters_cp adv_params_cp = {};
    adv_params_cp.min_interval = htobs(MIN_INTERVAL_MS/MS_PER_INTERVAL);
    adv_params_cp.max_interval = htobs(MAX_INTERVAL_MS/MS_PER_INTERVAL);
    adv_params_cp.advtype = 0x03; // ADV_NOCONN_IND
    adv_params_cp.own_bdaddr_type = 0x01; // Random device address
    adv_params_cp.chan_map = 0x07; // All three channels in use

    struct hci_request rq = {};
    rq.ogf = OGF_LE_CTL;
    rq.ocf = OCF_LE_SET_ADVERTISING_PARAMETERS;
    rq.cparam = &adv_params_cp;
    rq.clen = LE_SET_ADVERTISING_PARAMETERS_CP_SIZE;
    do_req(rq);

    /* TODO: Do we need to explicity do a random address msg here? See V4E, 7.8.52 */

    //
    // Enable advertising
    // NB: It's stated that enabling already-enabled advertising can cause the
    // random address to change. Even if we can confirm this behavior, we should
    // find an explicit method of doing so in case the behavior changes.
    //
    le_set_advertise_enable_cp advertise_cp = {};
    advertise_cp.enable = 0x01;

    rq = {};
    rq.ogf = OGF_LE_CTL;
    rq.ocf = OCF_LE_SET_ADVERTISE_ENABLE;
    rq.cparam = &advertise_cp;
    rq.clen = LE_SET_ADVERTISE_ENABLE_CP_SIZE;
    do_req(rq);

    //
    // Set advertising data
    //
    le_set_advertising_data_cp adv_data_cp = {};
    adv_data_cp.length = build_ct_packet(adv_data_cp.data,rpi);

    rq = {};
    rq.ogf = OGF_LE_CTL;
    rq.ocf = OCF_LE_SET_ADVERTISING_DATA;
    rq.cparam = &adv_data_cp;
    rq.clen = LE_SET_ADVERTISING_DATA_CP_SIZE;
    do_req(rq);
}

void CT_Beacon::stop_advertising() {

    le_set_advertise_enable_cp advertise_cp = {};

    struct hci_request rq = {};
    rq.ogf = OGF_LE_CTL;
    rq.ocf = OCF_LE_SET_ADVERTISE_ENABLE;
    rq.cparam = &advertise_cp;
    rq.clen = LE_SET_ADVERTISE_ENABLE_CP_SIZE;
    do_req(rq);
}

void CT_Beacon::start_listening() {
    // disable scanning
	if (hci_le_set_scan_enable(dev, 0x00, 0x00, 1000) < 0) 
        throw std::runtime_error("Could not enable LE scan.");

    // set filter
    struct hci_filter filter;
    hci_filter_clear(&filter);
    hci_filter_set_ptype(HCI_EVENT_PKT, &filter);
    hci_filter_set_event(EVT_LE_META_EVENT, &filter);
    if (setsockopt(dev, SOL_HCI, HCI_FILTER, &filter, sizeof(filter)) < 0)
        throw std::runtime_error("Could not set filter on socket.");

    // set scan parameters
    // scan type = 0 (passive, no PDUs sent)
    // interval = 0x40 (40ms)
    // window = 0x30 (30ms)
    // own_address = random (0x1)
    // scanning filter policy = 0 (everything not directed to another device)
    //
	if (hci_le_set_scan_parameters(dev, 0x00, htobs(0x40), htobs(0x30),
            0x01, 0x00, 1000) < 0) 
        throw std::runtime_error("Could not set LE scan parameters.");
    // enable scanning with duplicate filtering enabled
	if (hci_le_set_scan_enable(dev, 0x01, 0x01, 1000) < 0) 
        throw std::runtime_error("Could not enable LE scan.");
}

void CT_Beacon::stop_listening() {
	if (hci_le_set_scan_enable(dev, 0x00, 0x00, 1000) < 0) 
        throw std::runtime_error("Could not enable LE scan.");
}


int CT_Beacon::log_to_stream(std::ostream& out, int timeout_ms) {
    struct pollfd fds = { dev, POLLIN, 0 };
    int rv = poll(&fds, 1, timeout_ms); 
    if (rv < 0) throw new std::runtime_error("Error during poll.");
    if (rv > 0) {
        uint8_t buf[HCI_MAX_EVENT_SIZE];
        ssize_t len = read(dev, buf, HCI_MAX_EVENT_SIZE);
        evt_le_meta_event* mevt = (evt_le_meta_event*)(buf + 1 + HCI_EVENT_HDR_SIZE);
        if (mevt->subevent == 0x02) { // advertising report
            le_advertising_info* ad = (le_advertising_info*)(mevt->data + 1);
            len -= (uint8_t*)ad - buf;
            out.write((char*)ad->data,len);
        }
        return 1;
    }
    return 0;
}

int test_beacon_main() {
    std::vector<uint8_t> rpi(16);
    for (auto i = 0; i < 16; i++) rpi[i] = i;
    CT_Beacon beacon;
    beacon.start_advertising(rpi);
    std::cout << "Advertising started..." << std::flush;
    std::cin.get();
    beacon.stop_advertising();
    std::cout << "advertising stopped." << std::endl;
    return 0;
}

