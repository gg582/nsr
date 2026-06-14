# IP2Location setup for NSR GeoIP plugin

The `geoip` plugin resolves hop IP addresses to country and provider (ISP). It tries a local IP2Location binary database first, then falls back to a public web API.

## Installing libip2location

The `geoip` executable links against the system `libip2location` at build time.

### Ubuntu / Debian

```bash
sudo apt-get update
sudo apt-get install libip2location-dev
```

### Fedora

```bash
sudo dnf install ip2location-devel
```

### Arch Linux

```bash
sudo pacman -S ip2location-c
```

### Alpine Linux

```bash
sudo apk add ip2location-dev
```

### openSUSE

```bash
sudo zypper install libip2location-devel
```

## Downloading the IP2Location database

You need a binary `.bin` database from [IP2Location](https://www.ip2location.com). The free **IP2Location LITE** DB1 or DB3 works for country and ISP lookup.

1. Create an account at https://lite.ip2location.com.
2. Download the IPv4 or IPv6 binary package.
3. Extract the `.bin` file, for example to:

```bash
mkdir -p ~/.nsr
cp IP2LOCATION-LITE-DB3.IPV4.BIN ~/.nsr/IP2LOCATION-LITE-DB3.IPV4.BIN
```

## Configuring NSR

Add the database path to `~/.nsrconfig`:

```bash
echo 'IP2_LOCATION_DB=/home/YOURNAME/.nsr/IP2LOCATION-LITE-DB3.IPV4.BIN' >> ~/.nsrconfig
```

Use the absolute path. The `geoip` plugin reads this key from `~/.nsrconfig` at startup.

## Web fallback

If the database is not configured, cannot be opened, or does not contain an IP, the plugin queries `http://ip-api.com/json/<ip>?fields=status,country,countryCode,isp`.

* The web fallback has rate limits.
* It is meant as a convenience, not a production data source.
* For IPv6 targets, download an IPv6 database or rely on the web fallback.

## Enabling the plugin

```bash
make plugins-install
```

Start NSR, press `t` to open the Tools menu, move the cursor to `geoip`, and press `Enter` to enable it. The setting is saved to `~/.nsrconfig` as `NSR_PLUGIN_geoip=1`.
