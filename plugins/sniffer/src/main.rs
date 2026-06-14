use std::collections::VecDeque;
use std::net::IpAddr;
use std::sync::{Arc, Mutex, OnceLock};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};
use std::io::{self, BufRead, Write};
use serde::{Serialize, Deserialize};

const PROTO_FILTERS: &[&str] = &[
    "ALL", "TCP", "UDP", "ICMP", "HTTP", "HTTPS", "DNS", "SSH", "FTP", "SMTP", "TLS"
];

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[repr(u8)]
enum TransProto {
    Other = 0,
    Icmp = 1,
    Tcp = 2,
    Udp = 3,
}

impl TransProto {
    fn name(self) -> &'static str {
        match self {
            TransProto::Icmp => "ICMP",
            TransProto::Tcp => "TCP",
            TransProto::Udp => "UDP",
            TransProto::Other => "OTHER",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[repr(u8)]
enum AppProto {
    Unknown = 0,
    Http = 1,
    Https = 2,
    Dns = 3,
    Ssh = 4,
    Ftp = 5,
    Smtp = 6,
    Tls = 7,
}

impl AppProto {
    fn name(self) -> &'static str {
        match self {
            AppProto::Http => "HTTP",
            AppProto::Https => "HTTPS",
            AppProto::Dns => "DNS",
            AppProto::Ssh => "SSH",
            AppProto::Ftp => "FTP",
            AppProto::Smtp => "SMTP",
            AppProto::Tls => "TLS",
            AppProto::Unknown => "?",
        }
    }
}

#[derive(Debug, Clone)]
struct SniffPacket {
    ts_ms: u64,
    src_ip: String,
    dst_ip: String,
    src_port: u16,
    dst_port: u16,
    trans: TransProto,
    app: AppProto,
    tcp_flags: u8,
    payload_len: u32,
    ip_total_len: u32,
    info: String,
}

struct GlobalState {
    target_ip: String,
    target_host: String,
    telemetry_target_ip: String,
    is_ipv6: bool,
    running: bool,
    
    capture_thread: Option<std::thread::JoinHandle<()>>,
    
    packets: VecDeque<SniffPacket>,
    total_captured: u64,
    total_filtered: u64,
    
    focused_addr: String,
    error_no_priv: bool,
    error_invalid_ip: bool,
    
    filter_proto: String,
    filter_string: String,
    
    curl_thread_active: bool,
    
    show_dashboard: bool,
    inspect_ip: String,
    inspect_proto_idx: usize,
    inspect_filter: String,
    inspect_show_raw: bool,
    inspect_body_mode: usize,
    inspect_focus: usize,
    
    countdown_active: bool,
    countdown_start_ms: u64,
    
    state_show_raw: bool,
    state_body_mode: usize,
    
    last_payload: Vec<u8>,
    last_packet: Option<SniffPacket>,
}

impl GlobalState {
    fn new() -> Self {
        GlobalState {
            target_ip: String::new(),
            target_host: String::new(),
            telemetry_target_ip: String::new(),
            is_ipv6: false,
            running: false,
            capture_thread: None,
            packets: VecDeque::with_capacity(128),
            total_captured: 0,
            total_filtered: 0,
            focused_addr: String::new(),
            error_no_priv: false,
            error_invalid_ip: false,
            filter_proto: String::new(),
            filter_string: String::new(),
            curl_thread_active: false,
            show_dashboard: false,
            inspect_ip: String::new(),
            inspect_proto_idx: 0,
            inspect_filter: String::new(),
            inspect_show_raw: false,
            inspect_body_mode: 0,
            inspect_focus: 0,
            countdown_active: false,
            countdown_start_ms: 0,
            state_show_raw: false,
            state_body_mode: 0,
            last_payload: Vec::new(),
            last_packet: None,
        }
    }
}

static STATE: OnceLock<Mutex<GlobalState>> = OnceLock::new();
static RUNNING_FLAG: OnceLock<Arc<AtomicBool>> = OnceLock::new();

fn get_state() -> &'static Mutex<GlobalState> {
    STATE.get_or_init(|| Mutex::new(GlobalState::new()))
}

fn lock_state() -> std::sync::MutexGuard<'static, GlobalState> {
    get_state().lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn get_running_flag() -> &'static Arc<AtomicBool> {
    RUNNING_FLAG.get_or_init(|| Arc::new(AtomicBool::new(false)))
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

fn ip_is_loopback(ip: &str) -> bool {
    ip.parse::<IpAddr>().map(|addr| addr.is_loopback()).unwrap_or(false)
}

fn is_ip_input_char(c: char) -> bool {
    c.is_ascii_hexdigit() || c == '.' || c == ':'
}

#[cfg(unix)]
fn create_raw_socket() -> Result<socket2::Socket, std::io::Error> {
    use socket2::{Socket, Domain, Type, Protocol};
    let protocol = (libc::ETH_P_ALL as u16).to_be();
    let socket = Socket::new(
        Domain::from(libc::AF_PACKET),
        Type::from(libc::SOCK_RAW),
        Some(Protocol::from(protocol as i32)),
    )?;
    socket.set_read_timeout(Some(std::time::Duration::from_millis(100)))?;
    Ok(socket)
}

#[cfg(not(unix))]
fn create_raw_socket() -> Result<socket2::Socket, std::io::Error> {
    Err(std::io::Error::new(std::io::ErrorKind::Unsupported, "Raw socket only supported on Unix"))
}

fn stop_sniffer() {
    let running_flag = get_running_flag();
    
    let thread_to_join = {
        let mut state = lock_state();
        if !state.running {
            return;
        }
        state.running = false;
        running_flag.store(false, Ordering::Relaxed);
        let t = state.capture_thread.take();
        state.target_ip.clear();
        state.target_host.clear();
        state.show_dashboard = false;
        state.countdown_active = false;
        state.error_invalid_ip = false;
        t
    };
    
    if let Some(handle) = thread_to_join {
        let _ = handle.join();
    }
}

fn start_sniffer(target_ip: &str) -> bool {
    stop_sniffer();
    
    let running_flag = get_running_flag();
    
    let mut state = lock_state();
    state.error_no_priv = false;
    state.error_invalid_ip = false;
    state.countdown_active = false;
    
    let target_addr = match target_ip.parse::<IpAddr>() {
        Ok(addr) => addr,
        Err(_) => {
            state.error_invalid_ip = true;
            return false;
        }
    };

    if ip_is_loopback(target_ip) {
        state.error_invalid_ip = true;
        return false;
    }
    
    let resolved_ip = target_addr.to_string();
    let target_host = if resolved_ip == state.telemetry_target_ip && !state.target_host.is_empty() {
        state.target_host.clone()
    } else {
        resolved_ip.clone()
    };

    let socket = match create_raw_socket() {
        Ok(s) => s,
        Err(_) => {
            state.error_no_priv = true;
            return false;
        }
    };
    
    state.target_ip = resolved_ip.clone();
    state.target_host = target_host;
    state.is_ipv6 = target_addr.is_ipv6();
    state.running = true;
    state.total_captured = 0;
    state.total_filtered = 0;
    state.packets.clear();
    
    running_flag.store(true, Ordering::Relaxed);
    
    let running_clone = running_flag.clone();
    let resolved_ip_clone = resolved_ip.clone();
    let capture_ipv6 = target_addr.is_ipv6();
    
    let handle = std::thread::spawn(move || {
        use std::io::Read;
        let mut buf = [0u8; 65536];
        let mut socket = socket;
        while running_clone.load(Ordering::Relaxed) {
            match (&mut socket).read(&mut buf) {
                Ok(n) => {
                    if n < 14 { continue; }
                    let eth_type = u16::from_be_bytes([buf[12], buf[13]]);
                    let l3_data = &buf[14..n];
                    
                    if !capture_ipv6 && eth_type == 0x0800 {
                        analyze_ipv4(l3_data, &resolved_ip_clone);
                    } else if capture_ipv6 && eth_type == 0x86DD {
                        analyze_ipv6(l3_data, &resolved_ip_clone);
                    }
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    continue;
                }
                Err(_) => {
                    std::thread::sleep(std::time::Duration::from_millis(10));
                }
            }
        }
    });
    
    state.capture_thread = Some(handle);
    true
}

struct Ipv4Header {
    ihl: u8,
    tot_len: u16,
    protocol: u8,
    saddr: [u8; 4],
    daddr: [u8; 4],
}

fn parse_ipv4(data: &[u8]) -> Option<Ipv4Header> {
    if data.len() < 20 { return None; }
    if data[0] >> 4 != 4 { return None; }
    let ihl = data[0] & 0x0F;
    if ihl < 5 { return None; }
    let tot_len = u16::from_be_bytes([data[2], data[3]]);
    if tot_len < (ihl as u16) * 4 || data.len() < tot_len as usize { return None; }
    let protocol = data[9];
    let mut saddr = [0; 4];
    saddr.copy_from_slice(&data[12..16]);
    let mut daddr = [0; 4];
    daddr.copy_from_slice(&data[16..20]);
    Some(Ipv4Header { ihl, tot_len, protocol, saddr, daddr })
}

struct Ipv6Header {
    plen: u16,
    nxt: u8,
    src: [u8; 16],
    dst: [u8; 16],
}

fn parse_ipv6(data: &[u8]) -> Option<Ipv6Header> {
    if data.len() < 40 { return None; }
    if data[0] >> 4 != 6 { return None; }
    let plen = u16::from_be_bytes([data[4], data[5]]);
    if data.len() < 40 + plen as usize { return None; }
    let nxt = data[6];
    let mut src = [0; 16];
    src.copy_from_slice(&data[8..24]);
    let mut dst = [0; 16];
    dst.copy_from_slice(&data[24..40]);
    Some(Ipv6Header { plen, nxt, src, dst })
}

fn ipv6_transport_offset(data: &[u8], mut next: u8) -> Option<(u8, usize)> {
    let mut offset = 40usize;

    loop {
        match next {
            0 | 43 | 60 => {
                if data.len() < offset + 2 { return None; }
                next = data[offset];
                let hdr_len = (data[offset + 1] as usize + 1) * 8;
                if hdr_len == 0 || data.len() < offset + hdr_len { return None; }
                offset += hdr_len;
            }
            44 => {
                if data.len() < offset + 8 { return None; }
                next = data[offset];
                offset += 8;
            }
            51 => {
                if data.len() < offset + 2 { return None; }
                next = data[offset];
                let hdr_len = (data[offset + 1] as usize + 2) * 4;
                if hdr_len == 0 || data.len() < offset + hdr_len { return None; }
                offset += hdr_len;
            }
            _ => return Some((next, offset)),
        }
    }
}

struct TcpHeader {
    sport: u16,
    dport: u16,
    flags: u8,
    doff: u8,
}

fn parse_tcp(data: &[u8]) -> Option<TcpHeader> {
    if data.len() < 20 { return None; }
    let sport = u16::from_be_bytes([data[0], data[1]]);
    let dport = u16::from_be_bytes([data[2], data[3]]);
    let flags = data[13];
    let doff = (data[12] >> 4) & 0x0F;
    if doff < 5 || data.len() < (doff as usize) * 4 { return None; }
    Some(TcpHeader { sport, dport, flags, doff })
}

struct UdpHeader {
    sport: u16,
    dport: u16,
}

fn parse_udp(data: &[u8]) -> Option<UdpHeader> {
    if data.len() < 8 { return None; }
    let len = u16::from_be_bytes([data[4], data[5]]) as usize;
    if len < 8 || data.len() < len { return None; }
    let sport = u16::from_be_bytes([data[0], data[1]]);
    let dport = u16::from_be_bytes([data[2], data[3]]);
    Some(UdpHeader { sport, dport })
}

fn tcp_flags_str(flags: u8) -> String {
    let mut s = String::new();
    if flags & 0x01 != 0 { s.push('F'); }
    if flags & 0x02 != 0 { s.push('S'); }
    if flags & 0x04 != 0 { s.push('R'); }
    if flags & 0x08 != 0 { s.push('P'); }
    if flags & 0x10 != 0 { s.push('A'); }
    if flags & 0x20 != 0 { s.push('U'); }
    if s.is_empty() {
        "[-]".to_string()
    } else {
        format!("[{}]", s)
    }
}

fn detect_app_by_ports(sport: u16, dport: u16) -> AppProto {
    for &port in &[sport, dport] {
        match port {
            20 | 21 => return AppProto::Ftp,
            22 => return AppProto::Ssh,
            25 | 587 => return AppProto::Smtp,
            53 => return AppProto::Dns,
            80 => return AppProto::Http,
            443 => return AppProto::Https,
            _ => {}
        }
    }
    AppProto::Unknown
}

fn payload_looks_like_tls(payload: &[u8]) -> bool {
    if payload.len() < 3 { return false; }
    payload[0] == 0x16 && payload[1] == 0x03 && payload[2] <= 0x04
}

fn payload_looks_like_http(payload: &[u8]) -> bool {
    if payload.len() < 8 { return false; }
    let s = match std::str::from_utf8(&payload[..8.min(payload.len())]) {
        Ok(val) => val,
        Err(_) => return false,
    };
    s.starts_with("GET ") || s.starts_with("POST ") || s.starts_with("PUT ") ||
    s.starts_with("HTTP/") || s.starts_with("HEAD ")
}

fn payload_looks_like_dns(payload: &[u8]) -> bool {
    if payload.len() < 12 { return false; }
    let flags = u16::from_be_bytes([payload[2], payload[3]]);
    let opcode = (flags >> 11) & 0x0F;
    opcode <= 5
}

fn payload_looks_like_ssh(payload: &[u8]) -> bool {
    if payload.len() < 7 { return false; }
    payload.starts_with(b"SSH-2.0") || payload.starts_with(b"SSH-1.99")
}

fn parse_http_summary(payload: &[u8]) -> Option<String> {
    if payload.len() < 8 { return None; }
    let text = String::from_utf8_lossy(payload);
    let mut lines = text.lines();
    let first_line = lines.next()?;
    
    let is_request = first_line.starts_with("GET ") || first_line.starts_with("POST ") ||
                     first_line.starts_with("PUT ") || first_line.starts_with("DELETE ") ||
                     first_line.starts_with("HEAD ") || first_line.starts_with("OPTIONS ") ||
                     first_line.starts_with("PATCH ");
                     
    if is_request {
        let parts: Vec<&str> = first_line.split_whitespace().collect();
        if parts.len() < 2 { return None; }
        let method = parts[0];
        let path = parts[1];
        
        let mut host = "";
        let mut ua = "";
        for line in lines {
            let line_lower = line.to_lowercase();
            if line_lower.starts_with("host: ") {
                host = line["host: ".len()..].trim();
            } else if line_lower.starts_with("user-agent: ") {
                ua = line["user-agent: ".len()..].trim();
            }
        }
        
        let mut detail = format!("{} {}", method, path);
        if !host.is_empty() {
            detail.push_str(&format!(" ({})", host));
        }
        if !ua.is_empty() {
            let ua_short = if ua.chars().count() > 31 {
                ua.chars().take(31).collect::<String>()
            } else {
                ua.to_string()
            };
            detail.push_str(&format!(" UA={}", ua_short));
        }
        if detail.chars().count() > 120 {
            detail = detail.chars().take(120).collect();
        }
        return Some(detail);
    }
    
    if first_line.starts_with("HTTP/") {
        let parts: Vec<&str> = first_line.split_whitespace().collect();
        if parts.len() < 2 { return None; }
        let code = parts[1..].join(" ");
        
        let mut server = "";
        for line in lines {
            let line_lower = line.to_lowercase();
            if line_lower.starts_with("server: ") {
                server = line["server: ".len()..].trim();
            }
        }
        
        let mut detail = format!("RESP {}", code);
        if !server.is_empty() {
            let srv_short = if server.chars().count() > 31 {
                server.chars().take(31).collect::<String>()
            } else {
                server.to_string()
            };
            detail.push_str(&format!(" Srv={}", srv_short));
        }
        if detail.chars().count() > 120 {
            detail = detail.chars().take(120).collect();
        }
        return Some(detail);
    }
    
    None
}

fn parse_tls_server_hello(payload: &[u8]) -> Option<(String, u16)> {
    if payload.len() < 47 { return None; }
    if payload[0] != 0x16 { return None; }
    
    let rec_len = u16::from_be_bytes([payload[3], payload[4]]) as usize;
    if rec_len + 5 > payload.len() { return None; }
    if payload[5] != 0x02 { return None; }
    
    let hs_len = ((payload[6] as usize) << 16) | ((payload[7] as usize) << 8) | (payload[8] as usize);
    if hs_len + 9 > payload.len() { return None; }
    
    let mut offset = 9;
    let end = 9 + hs_len;
    
    if offset + 34 > end { return None; }
    offset += 34;
    
    if offset + 1 > end { return None; }
    let sid_len = payload[offset] as usize;
    offset += 1;
    if offset + sid_len > end { return None; }
    offset += sid_len;
    
    if offset + 2 > end { return None; }
    let cipher = u16::from_be_bytes([payload[offset], payload[offset+1]]);
    offset += 2;
    
    if offset + 1 > end { return None; }
    offset += 1;
    
    let mut tls_version = 0x0303;
    
    if offset + 2 <= end {
        let ext_len = u16::from_be_bytes([payload[offset], payload[offset+1]]) as usize;
        offset += 2;
        let ext_end = (offset + ext_len).min(end);
        
        while offset + 4 <= ext_end {
            let ext_type = u16::from_be_bytes([payload[offset], payload[offset+1]]);
            let ext_data_len = u16::from_be_bytes([payload[offset+2], payload[offset+3]]) as usize;
            offset += 4;
            if offset + ext_data_len > ext_end { break; }
            
            if ext_type == 0x002b {
                if ext_data_len == 2 {
                    tls_version = u16::from_be_bytes([payload[offset], payload[offset+1]]);
                }
            }
            offset += ext_data_len;
        }
    }
    
    let ver_str = match tls_version {
        0x0304 => "TLSv1.3",
        0x0303 => "TLSv1.2",
        0x0302 => "TLSv1.1",
        0x0301 => "TLSv1.0",
        _ => "TLS Unknown",
    }.to_string();
    
    Some((ver_str, cipher))
}

fn cipher_suite_name(cipher: u16) -> &'static str {
    match cipher {
        0x1301 => "AES_128_GCM_SHA256",
        0x1302 => "AES_256_GCM_SHA384",
        0x1303 => "CHACHA20_POLY1305_SHA256",
        0xc02b => "ECDHE_ECDSA_AES_128_GCM_SHA256",
        0xc02c => "ECDHE_ECDSA_AES_256_GCM_SHA384",
        0xc02f => "ECDHE_RSA_AES_128_GCM_SHA256",
        0xc030 => "ECDHE_RSA_AES_256_GCM_SHA384",
        _ => "UNKNOWN",
    }
}

fn parse_tls_client_hello(payload: &[u8]) -> Option<(String, bool)> {
    if payload.len() < 43 { return None; }
    if payload[0] != 0x16 { return None; }
    
    let rec_len = u16::from_be_bytes([payload[3], payload[4]]) as usize;
    if rec_len + 5 > payload.len() { return None; }
    if payload[5] != 0x01 { return None; }
    
    let hs_len = ((payload[6] as usize) << 16) | ((payload[7] as usize) << 8) | (payload[8] as usize);
    if hs_len + 9 > payload.len() { return None; }
    
    let mut offset = 9;
    let end = 9 + hs_len;
    
    if offset + 34 > end { return None; }
    offset += 34;
    
    if offset + 1 > end { return None; }
    let sid_len = payload[offset] as usize;
    offset += 1 + sid_len;
    
    if offset + 2 > end { return None; }
    let cipher_len = u16::from_be_bytes([payload[offset], payload[offset+1]]) as usize;
    offset += 2 + cipher_len;
    
    if offset + 1 > end { return None; }
    let comp_len = payload[offset] as usize;
    offset += 1 + comp_len;
    
    if offset + 2 > end { return None; }
    let ext_len = u16::from_be_bytes([payload[offset], payload[offset+1]]) as usize;
    offset += 2;
    let ext_end = (offset + ext_len).min(end);
    
    let mut sni_str = String::new();
    let mut ech = false;
    
    while offset + 4 <= ext_end {
        let ext_type = u16::from_be_bytes([payload[offset], payload[offset+1]]);
        let ext_data_len = u16::from_be_bytes([payload[offset+2], payload[offset+3]]) as usize;
        offset += 4;
        if offset + ext_data_len > ext_end { break; }
        
        let next_ext = offset + ext_data_len;
        
        if ext_type == 0x0000 {
            let mut p = offset;
            if p + 2 <= next_ext {
                let sni_list_len = u16::from_be_bytes([payload[p], payload[p+1]]) as usize;
                p += 2;
                if p + 1 + sni_list_len <= next_ext && sni_list_len > 0 {
                    let name_type = payload[p];
                    p += 1;
                    if name_type == 0 {
                        if p + 2 <= next_ext {
                            let name_len = u16::from_be_bytes([payload[p], payload[p+1]]) as usize;
                            p += 2;
                            if p + name_len <= next_ext {
                                sni_str = String::from_utf8_lossy(&payload[p..p+name_len]).into_owned();
                            }
                        }
                    }
                }
            }
        } else if ext_type == 0xfe0d || (ext_type >= 0xff03 && ext_type <= 0xff09) {
            ech = true;
        }
        
        offset = next_ext;
    }
    
    Some((sni_str, ech))
}

fn parse_dns_query_name(payload: &[u8]) -> Option<String> {
    if payload.len() < 12 { return None; }
    let qdcount = u16::from_be_bytes([payload[4], payload[5]]) as usize;
    if qdcount == 0 { return None; }
    
    let mut offset = 12;
    let mut name = String::new();
    
    while offset < payload.len() {
        let label_len = payload[offset];
        if label_len == 0 {
            break;
        }
        if (label_len & 0xC0) == 0xC0 {
            break;
        }
        let len = label_len as usize;
        if offset + 1 + len > payload.len() {
            return None;
        }
        if !name.is_empty() {
            name.push('.');
        }
        let label = String::from_utf8_lossy(&payload[offset+1..offset+1+len]);
        name.push_str(&label);
        offset += 1 + len;
    }
    
    if name.is_empty() { None } else { Some(name) }
}

fn parse_dns_response_ip(payload: &[u8]) -> Option<String> {
    if payload.len() < 12 { return None; }
    let qdcount = u16::from_be_bytes([payload[4], payload[5]]) as usize;
    let ancount = u16::from_be_bytes([payload[6], payload[7]]) as usize;
    if ancount == 0 { return None; }
    
    let mut offset = 12;
    
    for _ in 0..qdcount {
        while offset < payload.len() {
            let l = payload[offset];
            if l == 0 {
                offset += 1;
                break;
            }
            if (l & 0xC0) == 0xC0 {
                offset += 2;
                break;
            }
            offset += 1 + (l as usize);
        }
        if offset + 4 > payload.len() { return None; }
        offset += 4;
    }
    
    for _ in 0..ancount {
        while offset < payload.len() {
            let l = payload[offset];
            if l == 0 {
                offset += 1;
                break;
            }
            if (l & 0xC0) == 0xC0 {
                offset += 2;
                break;
            }
            offset += 1 + (l as usize);
        }
        
        if offset + 10 > payload.len() { return None; }
        let rtype = u16::from_be_bytes([payload[offset], payload[offset+1]]);
        let rdlength = u16::from_be_bytes([payload[offset+8], payload[offset+9]]) as usize;
        offset += 10;
        
        if offset + rdlength > payload.len() { return None; }
        
        if rtype == 1 && rdlength == 4 {
            let mut ip_bytes = [0; 4];
            ip_bytes.copy_from_slice(&payload[offset..offset+4]);
            return Some(std::net::Ipv4Addr::from(ip_bytes).to_string());
        } else if rtype == 28 && rdlength == 16 {
            let mut ip_bytes = [0; 16];
            ip_bytes.copy_from_slice(&payload[offset..offset+16]);
            return Some(std::net::Ipv6Addr::from(ip_bytes).to_string());
        }
        
        offset += rdlength;
    }
    
    None
}

fn build_info(pkt: &SniffPacket, payload: &[u8]) -> String {
    let tflags = tcp_flags_str(pkt.tcp_flags);
    let app = pkt.app.name();
    
    if pkt.trans == TransProto::Tcp && !payload.is_empty() {
        if pkt.app == AppProto::Http || pkt.app == AppProto::Https || payload_looks_like_http(payload) {
            if let Some(detail) = parse_http_summary(payload) {
                return if pkt.src_port != 0 && pkt.dst_port != 0 {
                    format!("{} {} {}->{} {}", app, tflags, pkt.src_port, pkt.dst_port, detail)
                } else {
                    format!("{} {} {}", app, tflags, detail)
                };
            }
        }
        
        if pkt.app == AppProto::Tls || pkt.app == AppProto::Https || payload_looks_like_tls(payload) {
            if let Some((sni, ech)) = parse_tls_client_hello(payload) {
                let ech_suffix = if ech { " [ECH]" } else { "" };
                return if !sni.is_empty() {
                    format!("{} {} {}->{} SNI={}{}", app, tflags, pkt.src_port, pkt.dst_port, sni, ech_suffix)
                } else {
                    format!("{} {} {}->{} TLS{}", app, tflags, pkt.src_port, pkt.dst_port, ech_suffix)
                };
            }
            
            if let Some((tls_ver, cipher)) = parse_tls_server_hello(payload) {
                return format!("{} {} {}->{} {} {}", app, tflags, pkt.src_port, pkt.dst_port, tls_ver, cipher_suite_name(cipher));
            }
        }
    }
    
    if pkt.app == AppProto::Dns && payload.len() >= 12 {
        let is_query = (payload[2] & 0x80) == 0;
        let ancount = u16::from_be_bytes([payload[6], payload[7]]);
        
        if let Some(domain) = parse_dns_query_name(payload) {
            return if is_query {
                format!("DNS Query: {}", domain)
            } else if let Some(ip_ans) = parse_dns_response_ip(payload) {
                format!("DNS Resp: {} -> {}", domain, ip_ans)
            } else {
                format!("DNS Resp: {} (ans={})", domain, ancount)
            };
        }
    }
    
    if pkt.trans == TransProto::Icmp && payload.len() >= 8 {
        let is_v6 = pkt.src_ip.contains(':');
        let icmp_detail = if is_v6 {
            let rtype = payload[0];
            let code = payload[1];
            match rtype {
                128 => "ICMPv6 Echo Req".to_string(),
                129 => "ICMPv6 Echo Reply".to_string(),
                1 => format!("ICMPv6 Dest Unreach code={}", code),
                3 => format!("ICMPv6 Time Exceeded code={}", code),
                135 => "ND Neighbor Solicit".to_string(),
                136 => "ND Neighbor Advert".to_string(),
                133 => "ND Router Solicit".to_string(),
                134 => "ND Router Advert".to_string(),
                _ => format!("ICMPv6 type={} code={}", rtype, code),
            }
        } else {
            let rtype = payload[0];
            let code = payload[1];
            let id = u16::from_be_bytes([payload[4], payload[5]]);
            let seq = u16::from_be_bytes([payload[6], payload[7]]);
            match rtype {
                0 => format!("ICMP Echo Reply id={} seq={}", id, seq),
                8 => format!("ICMP Echo Req id={} seq={}", id, seq),
                3 => format!("ICMP Dest Unreach code={}", code),
                11 => format!("ICMP Time Exceeded code={}", code),
                _ => format!("ICMP type={} code={}", rtype, code),
            }
        };
        return icmp_detail;
    }
    
    if pkt.trans == TransProto::Tcp {
        if pkt.src_port != 0 && pkt.dst_port != 0 {
            format!("{} {} {}->{} len={}", app, tflags, pkt.src_port, pkt.dst_port, payload.len())
        } else {
            format!("{} {} len={}", app, tflags, payload.len())
        }
    } else if pkt.trans == TransProto::Udp {
        if pkt.src_port != 0 && pkt.dst_port != 0 {
            format!("{} {}->{} len={}", app, pkt.src_port, pkt.dst_port, payload.len())
        } else {
            format!("{} len={}", app, payload.len())
        }
    } else {
        format!("{} len={}", pkt.trans.name(), payload.len())
    }
}

fn packet_matches_filters(state: &GlobalState, pkt: &SniffPacket, payload: &[u8]) -> bool {
    if !state.filter_proto.is_empty() && state.filter_proto != "ALL" {
        let mut match_found = false;
        let fp = state.filter_proto.to_uppercase();
        if fp == "TCP" && pkt.trans == TransProto::Tcp { match_found = true; }
        else if fp == "UDP" && pkt.trans == TransProto::Udp { match_found = true; }
        else if fp == "ICMP" && pkt.trans == TransProto::Icmp { match_found = true; }
        else if fp == "HTTP" && pkt.app == AppProto::Http { match_found = true; }
        else if fp == "HTTPS" && pkt.app == AppProto::Https { match_found = true; }
        else if fp == "DNS" && pkt.app == AppProto::Dns { match_found = true; }
        else if fp == "SSH" && pkt.app == AppProto::Ssh { match_found = true; }
        else if fp == "FTP" && pkt.app == AppProto::Ftp { match_found = true; }
        else if fp == "SMTP" && pkt.app == AppProto::Smtp { match_found = true; }
        else if fp == "TLS" && pkt.app == AppProto::Tls { match_found = true; }
        
        if !match_found { return false; }
    }
    
    if !state.filter_string.is_empty() {
        let mut match_found = false;
        let fs = state.filter_string.to_lowercase();
        
        if !payload.is_empty() {
            let payload_str = String::from_utf8_lossy(payload).to_lowercase();
            if payload_str.contains(&fs) {
                match_found = true;
            }
        }
        
        if !match_found && pkt.info.to_lowercase().contains(&fs) {
            match_found = true;
        }
        
        if !match_found { return false; }
    }
    
    true
}

fn refine_app_proto(app: AppProto, payload: &[u8]) -> AppProto {
    if payload.is_empty() { return app; }
    
    if app == AppProto::Https && payload_looks_like_tls(payload) {
        return AppProto::Tls;
    }
    
    if app == AppProto::Unknown {
        if payload_looks_like_http(payload) {
            return AppProto::Http;
        }
        if payload_looks_like_ssh(payload) {
            return AppProto::Ssh;
        }
        if payload_looks_like_tls(payload) {
            return AppProto::Tls;
        }
        if payload_looks_like_dns(payload) {
            return AppProto::Dns;
        }
    }
    
    app
}

fn analyze_ipv4(data: &[u8], target_ip: &str) {
    let ip = match parse_ipv4(data) {
        Some(header) => header,
        None => return,
    };
    
    let src_ip = std::net::Ipv4Addr::from(ip.saddr).to_string();
    let dst_ip = std::net::Ipv4Addr::from(ip.daddr).to_string();
    
    if src_ip != target_ip && dst_ip != target_ip {
        let mut state = lock_state();
        state.total_filtered += 1;
        return;
    }
    
    let ihl_bytes = (ip.ihl * 4) as usize;
    if data.len() < ihl_bytes { return; }
    
    let trans_data = &data[ihl_bytes..];
    let mut trans_proto = TransProto::Other;
    let mut sport = 0;
    let mut dport = 0;
    let mut tcp_flags = 0;
    let mut app_proto = AppProto::Unknown;
    let mut payload: &[u8] = &[];
    
    match ip.protocol {
        6 => {
            if let Some(tcp) = parse_tcp(trans_data) {
                trans_proto = TransProto::Tcp;
                sport = tcp.sport;
                dport = tcp.dport;
                tcp_flags = tcp.flags;
                let thl_bytes = (tcp.doff * 4) as usize;
                if trans_data.len() >= thl_bytes {
                    payload = &trans_data[thl_bytes..];
                }
                app_proto = detect_app_by_ports(sport, dport);
            } else { return; }
        }
        17 => {
            if let Some(udp) = parse_udp(trans_data) {
                trans_proto = TransProto::Udp;
                sport = udp.sport;
                dport = udp.dport;
                if trans_data.len() >= 8 {
                    payload = &trans_data[8..];
                }
                app_proto = detect_app_by_ports(sport, dport);
            } else { return; }
        }
        1 => {
            trans_proto = TransProto::Icmp;
            payload = trans_data;
        }
        _ => {
            payload = trans_data;
        }
    }
    
    let tot_len = ip.tot_len as usize;
    let mut payload_len = payload.len();
    if tot_len >= ihl_bytes {
        let mut limit = tot_len - ihl_bytes;
        if ip.protocol == 6 {
            if trans_data.len() >= 20 {
                let thl = ((trans_data[12] >> 4) & 0x0F) as usize * 4;
                limit = limit.saturating_sub(thl);
            }
        } else if ip.protocol == 17 {
            limit = limit.saturating_sub(8);
        }
        payload_len = payload_len.min(limit);
    } else {
        payload_len = 0;
    }
    
    let final_payload = &payload[..payload_len];
    app_proto = refine_app_proto(app_proto, final_payload);
    
    let mut pkt = SniffPacket {
        ts_ms: now_ms(),
        src_ip,
        dst_ip,
        src_port: sport,
        dst_port: dport,
        trans: trans_proto,
        app: app_proto,
        tcp_flags,
        payload_len: payload_len as u32,
        ip_total_len: tot_len as u32,
        info: String::new(),
    };
    
    pkt.info = build_info(&pkt, final_payload);
    
    let mut state = lock_state();
    if !packet_matches_filters(&state, &pkt, final_payload) {
        state.total_filtered += 1;
        return;
    }
    
    state.total_captured += 1;
    state.last_packet = Some(pkt.clone());
    state.last_payload = final_payload.to_vec();
    state.packets.push_back(pkt);
    if state.packets.len() > 128 {
        state.packets.pop_front();
    }
}

fn analyze_ipv6(data: &[u8], target_ip: &str) {
    let ip6 = match parse_ipv6(data) {
        Some(header) => header,
        None => return,
    };
    
    let src_ip = std::net::Ipv6Addr::from(ip6.src).to_string();
    let dst_ip = std::net::Ipv6Addr::from(ip6.dst).to_string();
    
    if src_ip != target_ip && dst_ip != target_ip {
        let mut state = lock_state();
        state.total_filtered += 1;
        return;
    }
    
    let (next_header, trans_offset) = match ipv6_transport_offset(data, ip6.nxt) {
        Some(v) => v,
        None => return,
    };
    let trans_data = &data[trans_offset..];
    
    let mut trans_proto = TransProto::Other;
    let mut sport = 0;
    let mut dport = 0;
    let mut tcp_flags = 0;
    let mut app_proto = AppProto::Unknown;
    let mut payload: &[u8] = &[];
    
    match next_header {
        6 => {
            if let Some(tcp) = parse_tcp(trans_data) {
                trans_proto = TransProto::Tcp;
                sport = tcp.sport;
                dport = tcp.dport;
                tcp_flags = tcp.flags;
                let thl_bytes = (tcp.doff * 4) as usize;
                if trans_data.len() >= thl_bytes {
                    payload = &trans_data[thl_bytes..];
                }
                app_proto = detect_app_by_ports(sport, dport);
            } else { return; }
        }
        17 => {
            if let Some(udp) = parse_udp(trans_data) {
                trans_proto = TransProto::Udp;
                sport = udp.sport;
                dport = udp.dport;
                if trans_data.len() >= 8 {
                    payload = &trans_data[8..];
                }
                app_proto = detect_app_by_ports(sport, dport);
            } else { return; }
        }
        58 => {
            trans_proto = TransProto::Icmp;
            payload = trans_data;
        }
        _ => {
            payload = trans_data;
        }
    }
    
    let plen = ip6.plen as usize;
    let mut limit = plen;
    if next_header == 6 {
        if trans_data.len() >= 20 {
            let thl = ((trans_data[12] >> 4) & 0x0F) as usize * 4;
            limit = limit.saturating_sub(thl);
        }
    } else if next_header == 17 {
        limit = limit.saturating_sub(8);
    }
    let payload_len = payload.len().min(limit);
    let final_payload = &payload[..payload_len];
    
    app_proto = refine_app_proto(app_proto, final_payload);
    
    let mut pkt = SniffPacket {
        ts_ms: now_ms(),
        src_ip,
        dst_ip,
        src_port: sport,
        dst_port: dport,
        trans: trans_proto,
        app: app_proto,
        tcp_flags,
        payload_len: payload_len as u32,
        ip_total_len: (40 + plen) as u32,
        info: String::new(),
    };
    
    pkt.info = build_info(&pkt, final_payload);
    
    let mut state = lock_state();
    if !packet_matches_filters(&state, &pkt, final_payload) {
        state.total_filtered += 1;
        return;
    }
    
    state.total_captured += 1;
    state.last_packet = Some(pkt.clone());
    state.last_payload = final_payload.to_vec();
    state.packets.push_back(pkt);
    if state.packets.len() > 128 {
        state.packets.pop_front();
    }
}

fn write_hex_dump(f: &mut dyn std::io::Write, data: &[u8]) -> std::io::Result<()> {
    let len = data.len();
    for i in (0..len).step_by(16) {
        write!(f, "{:08x}: ", i)?;
        for j in 0..16 {
            if i + j < len {
                write!(f, "{:02x} ", data[i + j])?;
            } else {
                write!(f, "   ")?;
            }
        }
        write!(f, " ")?;
        for j in 0..16 {
            if i + j < len {
                let c = data[i + j];
                if c.is_ascii() && !c.is_ascii_control() {
                    write!(f, "{}", c as char)?;
                } else {
                    write!(f, ".")?;
                }
            }
        }
        writeln!(f)?;
    }
    Ok(())
}

fn write_placeholder_packet_file() -> bool {
    use std::fs::File;
    use std::io::Write;
    let mut f = match File::create("/tmp/nsr_last_packet.txt") {
        Ok(file) => file,
        Err(_) => return false,
    };
    let _ = writeln!(f, "==================================================");
    let _ = writeln!(f, "                NSR PACKET INSPECTION              ");
    let _ = writeln!(f, "==================================================\n");
    let _ = writeln!(f, "No packet captured yet.\n");
    let _ = writeln!(f, "Instructions:");
    let _ = writeln!(f, "1. Focus a hop in the TUI normal mode.");
    let _ = writeln!(f, "2. Press [f] to start sniffing that hop.");
    let _ = writeln!(f, "3. Press [a] to configure packet & payload filters.");
    let _ = writeln!(f, "==================================================");
    true
}

fn write_last_packet_to_file(pkt: &SniffPacket) -> bool {
    use std::fs::File;
    use std::io::Write;
    let mut f = match File::create("/tmp/nsr_last_packet.txt") {
        Ok(file) => file,
        Err(_) => return false,
    };
    
    let sec = (pkt.ts_ms / 1000) as i64;
    let time_str = unsafe {
        let mut t = sec as libc::time_t;
        let tm = libc::localtime(&mut t);
        if !tm.is_null() {
            let year = (*tm).tm_year + 1900;
            let mon = (*tm).tm_mon + 1;
            let mday = (*tm).tm_mday;
            let hour = (*tm).tm_hour;
            let min = (*tm).tm_min;
            let sec_val = (*tm).tm_sec;
            format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", year, mon, mday, hour, min, sec_val)
        } else {
            "unknown".to_string()
        }
    };
    
    let _ = writeln!(f, "==================================================");
    let _ = writeln!(f, "                NSR PACKET INSPECTION              ");
    let _ = writeln!(f, "==================================================\n");
    let _ = writeln!(f, "Timestamp:        {}.{:03}", time_str, pkt.ts_ms % 1000);
    let _ = writeln!(f, "Source IP:        {}", pkt.src_ip);
    let _ = writeln!(f, "Destination IP:   {}", pkt.dst_ip);
    let _ = writeln!(f, "Transport Proto:  {}", pkt.trans.name());
    if pkt.trans == TransProto::Tcp || pkt.trans == TransProto::Udp {
        let _ = writeln!(f, "Source Port:      {}", pkt.src_port);
        let _ = writeln!(f, "Destination Port: {}", pkt.dst_port);
    }
    if pkt.trans == TransProto::Tcp {
        let _ = writeln!(f, "TCP Flags:        {}", tcp_flags_str(pkt.tcp_flags));
    }
    let _ = writeln!(f, "App Protocol:     {}", pkt.app.name());
    let _ = writeln!(f, "IP Total Length:  {} bytes", pkt.ip_total_len);
    let _ = writeln!(f, "Payload Length:   {} bytes\n", pkt.payload_len);
    let _ = writeln!(f, "Packet Summary:\n{}\n", pkt.info);
    let _ = writeln!(f, "==================================================");
    
    true
}

fn perform_curl_request(url: String) {
    std::thread::spawn(move || {
        let mut handle = curl::easy::Easy::new();
        if handle.url(&url).is_ok() {
            let _ = handle.nobody(false);
            let _ = handle.follow_location(true);
            let _ = handle.ssl_verify_peer(false);
            let _ = handle.ssl_verify_host(false);
            let _ = handle.timeout(std::time::Duration::from_secs(5));
            let _ = handle.perform();
        }
        let mut state = lock_state();
        state.curl_thread_active = false;
    });
}

fn send_response(id: Option<i64>, result: serde_json::Value) {
    let mut resp = serde_json::json!({
        "jsonrpc": "2.0",
        "result": result
    });
    if let Some(req_id) = id {
        if let Some(obj) = resp.as_object_mut() {
            obj.insert("id".to_string(), serde_json::Value::Number(req_id.into()));
        }
    }
    if let Ok(json_str) = serde_json::to_string(&resp) {
        println!("{}", json_str);
        let _ = std::io::stdout().flush();
    }
}

fn send_error(id: Option<i64>, code: i32, message: &str) {
    let mut resp = serde_json::json!({
        "jsonrpc": "2.0",
        "error": {
            "code": code,
            "message": message
        }
    });
    if let Some(req_id) = id {
        if let Some(obj) = resp.as_object_mut() {
            obj.insert("id".to_string(), serde_json::Value::Number(req_id.into()));
        }
    }
    if let Ok(json_str) = serde_json::to_string(&resp) {
        println!("{}", json_str);
        let _ = std::io::stdout().flush();
    }
}

fn send_key_response(id: Option<i64>, handled: bool) {
    let modal_active = {
        let state = lock_state();
        state.show_dashboard || state.countdown_active
    };
    send_response(id, serde_json::json!({
        "handled": handled,
        "modal_active": modal_active,
        "is_modal": modal_active
    }));
}

fn handle_init(id: Option<i64>) {
    send_response(id, serde_json::json!({
        "status": "ok",
        "description": "Lightweight DPI packet sniffer (press f on a hop)",
        "reserved_keys": "fas"
    }));
}

fn handle_update_telemetry(params: &serde_json::Value) {
    let mut state = lock_state();
    if let Some(target) = params.get("target_ip").and_then(|v| v.as_str()) {
        state.telemetry_target_ip = target.to_string();
    }
    if let Some(host) = params.get("target_host").and_then(|v| v.as_str()) {
        state.target_host = host.to_string();
    }
}

fn handle_cleanup() {
    stop_sniffer();
    let mut state = lock_state();
    state.show_dashboard = false;
    state.inspect_ip.clear();
    state.inspect_filter.clear();
    state.inspect_focus = 0;
    state.countdown_active = false;
}

fn handle_render(id: Option<i64>, params: &serde_json::Value) {
    let mut focused_addr = String::new();
    if let Some(addr) = params.get("focused_addr").and_then(|v| v.as_str()) {
        focused_addr = addr.to_string();
    }
    
    let mut width = 60i64;
    if let Some(w) = params.get("width").and_then(|v| v.as_i64()) {
        width = w;
    }
    if width < 20 { width = 60; }
    
    let mut height = 18i64;
    if let Some(h) = params.get("height").and_then(|v| v.as_i64()) {
        height = h;
    }
    if height < 6 { height = 6; }
    
    let mut show_countdown_modal = false;
    let mut countdown_rem_secs = 0;
    
    {
        let mut state = lock_state();
        state.focused_addr = focused_addr;
        
        if state.countdown_active && !state.show_dashboard {
            let elapsed = now_ms().saturating_sub(state.countdown_start_ms);
            if elapsed >= 3000 {
                let host_buf = if !state.target_host.is_empty() {
                    if state.target_host.contains(':') {
                        format!("[{}]", state.target_host)
                    } else {
                        state.target_host.clone()
                    }
                } else if !state.target_ip.is_empty() {
                    if state.target_ip.contains(':') {
                        format!("[{}]", state.target_ip)
                    } else {
                        state.target_ip.clone()
                    }
                } else {
                    String::new()
                };
                
                if !host_buf.is_empty() {
                    let proto_scheme = if state.filter_proto.to_uppercase() == "HTTP" { "http" } else { "https" };
                    let url = format!("{}://{}", proto_scheme, host_buf);
                    
                    if !state.curl_thread_active {
                        state.curl_thread_active = true;
                        perform_curl_request(url);
                    }
                }
                state.countdown_active = false;
            } else {
                show_countdown_modal = true;
                countdown_rem_secs = 3 - (elapsed / 1000) as i32;
                if countdown_rem_secs < 1 { countdown_rem_secs = 1; }
            }
        }
    }
    
    if show_countdown_modal {
        let msg = format!("CURL Request Accepted. Starting in {}s...", countdown_rem_secs);
        let mut x_pos = (width - msg.len() as i64) / 2;
        if x_pos < 1 { x_pos = 1; }
        
        let lines = vec![
            serde_json::json!({
                "y": 2,
                "x": x_pos,
                "text": msg,
                "color": "yellow"
            })
        ];
        
        send_response(id, serde_json::json!({
            "is_modal": true,
            "modal_active": true,
            "modal_width": 60,
            "modal_height": 5,
            "lines": lines
        }));
        return;
    }
    
    let show_dashboard_local = {
        let state = lock_state();
        state.show_dashboard
    };
    
    if show_dashboard_local {
        let state = lock_state();
        let mut lines = Vec::new();
        let mut y = 1;
        
        lines.push(serde_json::json!({
            "y": y,
            "x": 12,
            "text": "=== PACKET INSPECTION SETTINGS ===",
            "color": "cyan"
        }));
        y += 2;
        
        let cursor_ip = if state.inspect_focus == 0 { "_" } else { "" };
        let marker_ip = if state.inspect_focus == 0 { " >" } else { "  " };
        lines.push(serde_json::json!({
            "y": y,
            "x": 2,
            "text": format!("{} Target IP:        {}{}", marker_ip, state.inspect_ip, cursor_ip),
            "color": if state.inspect_focus == 0 { Some("yellow") } else { None }
        }));
        y += 1;
        
        let marker_proto = if state.inspect_focus == 1 { " >" } else { "  " };
        lines.push(serde_json::json!({
            "y": y,
            "x": 2,
            "text": format!("{} Protocol Filter: < {} >", marker_proto, PROTO_FILTERS[state.inspect_proto_idx]),
            "color": if state.inspect_focus == 1 { Some("yellow") } else { None }
        }));
        y += 1;
        
        let cursor_filter = if state.inspect_focus == 2 { "_" } else { "" };
        let marker_filter = if state.inspect_focus == 2 { " >" } else { "  " };
        lines.push(serde_json::json!({
            "y": y,
            "x": 2,
            "text": format!("{} Payload Filter:  {}{}", marker_filter, state.inspect_filter, cursor_filter),
            "color": if state.inspect_focus == 2 { Some("yellow") } else { None }
        }));
        y += 1;
        
        let marker_raw = if state.inspect_focus == 3 { " >" } else { "  " };
        let raw_val = if state.inspect_show_raw { "Yes" } else { "No " };
        lines.push(serde_json::json!({
            "y": y,
            "x": 2,
            "text": format!("{} Show Raw Body:    [ {} ]", marker_raw, raw_val),
            "color": if state.inspect_focus == 3 { Some("yellow") } else { None }
        }));
        y += 1;
        
        let marker_mode = if state.inspect_focus == 4 { " >" } else { "  " };
        let mode_val = if state.inspect_body_mode == 0 { "UTF-8" } else { "Hex  " };
        lines.push(serde_json::json!({
            "y": y,
            "x": 2,
            "text": format!("{} Body Mode:        < {} >", marker_mode, mode_val),
            "color": if state.inspect_focus == 4 { Some("yellow") } else { None }
        }));
        y += 2;
        
        lines.push(serde_json::json!({
            "y": y, "x": 4, "text": "Use [Up/Down] to navigate fields", "color": "white"
        }));
        y += 1;
        lines.push(serde_json::json!({
            "y": y, "x": 4, "text": "Use [Left/Right] to change Protocol/Raw/Mode", "color": "white"
        }));
        y += 1;
        
        lines.push(serde_json::json!({
            "y": y, "x": 4, "text": "Press ENTER to APPLY & CLOSE", "color": "green"
        }));
        y += 1;
        
        lines.push(serde_json::json!({
            "y": y, "x": 4, "text": "Press ESC to Cancel", "color": "red"
        }));
        
        send_response(id, serde_json::json!({
            "is_modal": true,
            "modal_active": true,
            "modal_width": 60,
            "modal_height": 15,
            "lines": lines
        }));
        return;
    }
    
    let state = lock_state();
    let mut lines = Vec::new();
    let mut y = 1;
    
    lines.push(serde_json::json!({
        "y": y,
        "x": 1,
        "text": "== Pckt Sniffer ==",
        "color": "cyan"
    }));
    y += 1;
    
    if state.running && !state.target_ip.is_empty() {
        lines.push(serde_json::json!({
            "y": y,
            "x": 1,
            "text": format!("Sniffing: {}", state.target_ip),
            "color": "green"
        }));
        y += 1;
        
        lines.push(serde_json::json!({
            "y": y,
            "x": 1,
            "text": format!("Captured: {}  Filtered: {}", state.total_captured, state.total_filtered),
            "color": "white"
        }));
        y += 1;
    } else {
        let msg = if state.error_invalid_ip {
            "Error: enter a valid IPv4 or IPv6 address".to_string()
        } else if state.error_no_priv {
            "Error: raw socket failed (need root/cap_net_raw)".to_string()
        } else if !state.focused_addr.is_empty() {
            format!("Focus: {}  Press [f] to sniff", state.focused_addr)
        } else {
            "Focus a hop, then press [f] to sniff".to_string()
        };
        let color = if state.error_no_priv { "red" } else { "yellow" };
        lines.push(serde_json::json!({
            "y": y,
            "x": 1,
            "text": msg,
            "color": color
        }));
        y += 1;
    }
    
    y += 1;
    
    if state.packets.is_empty() {
        lines.push(serde_json::json!({
            "y": y,
            "x": 1,
            "text": "No packets yet...",
            "color": "white"
        }));
    } else {
        let max_chars = (width - 2).max(0) as usize;
        for p in state.packets.iter() {
            if y >= height - 1 { break; }
            
            let mut line = format!("{} -> {} | {}", p.src_ip, p.dst_ip, p.info);
            if line.chars().count() > max_chars {
                line = line.chars().take(max_chars).collect();
            }
            
            let color = match p.trans {
                TransProto::Tcp => "cyan",
                TransProto::Udp => "magenta",
                _ => "white"
            };
            
            lines.push(serde_json::json!({
                "y": y,
                "x": 1,
                "text": line,
                "color": color
            }));
            y += 1;
        }
    }
    
    send_response(id, serde_json::json!({
        "lines": lines,
        "is_modal": false
    }));
}

fn handle_on_key(id: Option<i64>, params: &serde_json::Value) {
    let key = match params.get("key").and_then(|v| v.as_i64()) {
        Some(k) => k,
        None => {
            send_key_response(id, false);
            return;
        }
    };
    
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open("/tmp/sniffer_debug.log") {
        let _ = writeln!(f, "Received key: {}", key);
    }

    let mut handled = false;
    
    let show_dashboard_local = {
        let state = lock_state();
        state.show_dashboard
    };
    
    if show_dashboard_local {
        let apply_settings = {
            let mut state = lock_state();
            let mut apply = false;
            
            if key == 27 {
                state.show_dashboard = false;
                handled = true;
            } else if key == 259 || key == 'k' as i64 || key == 'K' as i64 {
                state.inspect_focus = (state.inspect_focus + 4) % 5;
                handled = true;
            } else if key == 258 || key == 'j' as i64 || key == 'J' as i64 {
                state.inspect_focus = (state.inspect_focus + 1) % 5;
                handled = true;
            } else if key == 260 || key == 'h' as i64 || key == 'H' as i64 {
                if state.inspect_focus == 1 {
                    let count = PROTO_FILTERS.len();
                    state.inspect_proto_idx = (state.inspect_proto_idx + count - 1) % count;
                } else if state.inspect_focus == 3 {
                    state.inspect_show_raw = !state.inspect_show_raw;
                } else if state.inspect_focus == 4 {
                    state.inspect_body_mode = 1 - state.inspect_body_mode;
                }
                handled = true;
            } else if key == 261 || key == 'l' as i64 || key == 'L' as i64 {
                if state.inspect_focus == 1 {
                    let count = PROTO_FILTERS.len();
                    state.inspect_proto_idx = (state.inspect_proto_idx + 1) % count;
                } else if state.inspect_focus == 3 {
                    state.inspect_show_raw = !state.inspect_show_raw;
                } else if state.inspect_focus == 4 {
                    state.inspect_body_mode = 1 - state.inspect_body_mode;
                }
                handled = true;
            } else if key == 343 || key == 10 || key == 13 {
                state.show_dashboard = false;
                apply = true;
                handled = true;
            } else if key == 263 || key == 127 || key == 8 {
                if state.inspect_focus == 0 {
                    let _ = state.inspect_ip.pop();
                } else if state.inspect_focus == 2 {
                    let _ = state.inspect_filter.pop();
                }
                handled = true;
            } else if key >= 32 && key <= 126 {
                let c = key as u8 as char;
                if state.inspect_focus == 0 {
                    state.error_invalid_ip = false;
                    if state.inspect_ip.len() < 47 && is_ip_input_char(c) {
                        state.inspect_ip.push(c);
                    }
                } else if state.inspect_focus == 2 {
                    if state.inspect_filter.len() < 63 {
                        state.inspect_filter.push(c);
                    }
                }
                handled = true;
            }
            apply
        };
        
        if apply_settings {
            let ip_to_sniff = {
                let mut state = lock_state();
                
                state.filter_proto = PROTO_FILTERS[state.inspect_proto_idx].to_string();
                state.filter_string = state.inspect_filter.clone();
                state.state_show_raw = state.inspect_show_raw;
                state.state_body_mode = state.inspect_body_mode;
                
                let fp = state.filter_proto.to_uppercase();
                if fp == "HTTP" || fp == "HTTPS" || fp == "TLS" {
                    state.countdown_active = true;
                    state.countdown_start_ms = now_ms();
                } else {
                    state.countdown_active = false;
                }
                
                state.inspect_ip.clone()
            };
            
            if !ip_to_sniff.is_empty() {
                let _ = start_sniffer(&ip_to_sniff);
            } else {
                stop_sniffer();
            }
        }
        
        send_key_response(id, handled);
        return;
    }
    
    if key == 'a' as i64 || key == 'A' as i64 {
        {
            let mut state = lock_state();
            state.show_dashboard = true;
            state.countdown_active = false;
            state.inspect_focus = 0;
            
            state.inspect_ip = if !state.target_ip.is_empty() {
                state.target_ip.clone()
            } else if !state.focused_addr.is_empty() {
                state.focused_addr.clone()
            } else {
                String::new()
            };

            state.inspect_filter = state.filter_string.clone();
            state.inspect_show_raw = state.state_show_raw;
            state.inspect_body_mode = state.state_body_mode;

            state.inspect_proto_idx = PROTO_FILTERS.iter()
                .position(|&p| p.eq_ignore_ascii_case(&state.filter_proto))
                .unwrap_or(0);
        }
        
        send_key_response(id, true);
        return;
    }
    
    if key == 's' as i64 {
        let mut has_packet = false;
        let mut last_pkt = None;
        let mut show_raw = false;
        let mut body_mode = 0;
        let mut payload_data = Vec::new();
        
        {
            let state = lock_state();
            if let Some(pkt) = &state.last_packet {
                has_packet = true;
                last_pkt = Some(pkt.clone());
                show_raw = state.state_show_raw;
                body_mode = state.state_body_mode;
                payload_data = state.last_payload.clone();
            }
        }
        
        if has_packet {
            if show_raw {
                use std::fs::File;
                if let Ok(mut f) = File::create("/tmp/nsr_last_packet.txt") {
                    if !payload_data.is_empty() {
                        if body_mode == 1 {
                            let _ = write_hex_dump(&mut f, &payload_data);
                        } else {
                            let _ = f.write_all(&payload_data);
                        }
                    } else {
                        let _ = writeln!(f, "(No payload data captured)");
                    }
                }
            } else {
                if let Some(pkt) = last_pkt {
                    let _ = write_last_packet_to_file(&pkt);
                }
            }
        } else {
            let _ = write_placeholder_packet_file();
        }
        
        send_response(id, serde_json::json!({
            "handled": true,
            "modal_active": false,
            "is_modal": false,
            "action": "open_editor",
            "file": "/tmp/nsr_last_packet.txt"
        }));
        return;
    }
    
    if key == 'f' as i64 || key == 'F' as i64 {
        let addr = {
            let state = lock_state();
            state.focused_addr.clone()
        };
        
        if !addr.is_empty() {
            let _ = start_sniffer(&addr);
            send_key_response(id, true);
            return;
        }
    }
    
    send_key_response(id, false);
}

fn main() {
    let stdin = io::stdin();
    let reader = stdin.lock();
    
    for line in reader.lines() {
        let line = match line {
            Ok(l) => l,
            Err(_) => break,
        };
        let line_trimmed = line.trim();
        if line_trimmed.is_empty() { continue; }
        
        let parsed: serde_json::Value = match serde_json::from_str(line_trimmed) {
            Ok(v) => v,
            Err(_) => continue,
        };
        
        let method = match parsed.get("method").and_then(|v| v.as_str()) {
            Some(m) => m,
            None => continue,
        };
        
        let params = parsed.get("params").unwrap_or(&serde_json::Value::Null);
        let id = parsed.get("id").and_then(|v| v.as_i64());
        
        match method {
            "init" => {
                handle_init(id);
            }
            "update_telemetry" => {
                handle_update_telemetry(params);
            }
            "render" => {
                handle_render(id, params);
            }
            "on_key" => {
                handle_on_key(id, params);
            }
            "cleanup" => {
                handle_cleanup();
            }
            _ => {
                if id.is_some() {
                    send_error(id, -32601, "Method not found");
                }
            }
        }
    }
    
    stop_sniffer();
}
