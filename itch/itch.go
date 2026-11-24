// SPDX-License-Identifier: GPL-2.0-or-later
// Guest download protocol adapted from q2-pico8 and Pocket Boy's itch.io helper.
// See NOTICE and NextUI-Itchio-Pak-LICENSE.
package main

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"

	"io"
	"net/http"
	"net/http/cookiejar"
	"net/url"

	"regexp"

	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"golang.org/x/net/html"
)

const maxArchive = 48 << 20
const maxPage = 2 << 20
const agent = "q2-playdate (https://github.com/lampsonlnguyen/q2-playdate)"

var (
	errVisit   = errors.New("Open the itch.io page for this download")
	errNoBundle = errors.New("No supported Lua Playdate bundle in the free files")
	errConnect = errors.New("Could not reach itch.io. Check Q2 Wi-Fi")
	errExists  = errors.New("This game is already in your library")
	gameSlug   = regexp.MustCompile(`^/[a-zA-Z0-9_-]+/?$`)
	csrfTag    = regexp.MustCompile(`name="csrf_token"\s+(?:content|value)="([^"]+)"`)
)

func permitted(u *url.URL) bool {
	if u == nil || u.Scheme != "https" || u.User != nil || u.Port() != "" {
		return false
	}
	h := u.Hostname()
	return h == "itch.io" || strings.HasSuffix(h, ".itch.io") ||
		h == "itch.zone" || strings.HasSuffix(h, ".itch.zone") ||
		h == "hwcdn.net" || strings.HasSuffix(h, ".hwcdn.net") ||
		(strings.HasPrefix(h, "itchio-") && strings.HasSuffix(h, ".r2.cloudflarestorage.com"))
}

func project(raw string) (*url.URL, bool) {
	u, err := url.Parse(raw)
	return u, err == nil && permitted(u) && strings.HasSuffix(u.Hostname(), ".itch.io") &&
		gameSlug.MatchString(u.Path) && u.RawQuery == "" && u.Fragment == ""
}

func newClient() *http.Client {
	jar, _ := cookiejar.New(nil)
	return &http.Client{Jar: jar, Timeout: 18 * time.Second, CheckRedirect: func(r *http.Request, via []*http.Request) error {
		if len(via) >= 5 || !permitted(r.URL) {
			return errVisit
		}
		return nil
	}}
}

func fetch(c *http.Client, address string, form url.Values, referer string) (*http.Response, error) {
	method := http.MethodGet
	var body io.Reader
	if form != nil {
		method = http.MethodPost
		body = strings.NewReader(form.Encode())
	}
	req, err := http.NewRequestWithContext(session, method, address, body)
	if err != nil || !permitted(req.URL) {
		return nil, errVisit
	}
	req.Header.Set("User-Agent", agent)
	if form != nil || referer != "" {
		req.Header.Set("Cache-Control", "no-cache")
	}
	if referer != "" {
		req.Header.Set("Referer", referer)
	}
	if form != nil {
		req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
		req.Header.Set("X-Requested-With", "XMLHttpRequest")
		req.Header.Set("Origin", req.URL.Scheme+"://"+req.URL.Host)
	}
	res, err := c.Do(req)
	if err != nil {
		if errors.Is(err, errVisit) {
			return nil, errVisit
		}
		return nil, errConnect
	}
	if res.StatusCode != http.StatusOK {
		res.Body.Close()
		if res.StatusCode == 429 || res.StatusCode >= 500 {
			return nil, errors.New("itch.io is busy. Try again later")
		}
		return nil, errVisit
	}
	return res, nil
}

func limited(r io.Reader, limit int64) ([]byte, error) {
	b, err := io.ReadAll(io.LimitReader(r, limit+1))
	if err != nil {
		return nil, errConnect
	}
	if int64(len(b)) > limit {
		return nil, errors.New("This file is too large for the Q2")
	}
	return b, nil
}

func pageLimit(c *http.Client, address string, form url.Values, referer string, limit int64) ([]byte, error) {
	res, err := fetch(c, address, form, referer)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()
	return limited(res.Body, limit)
}

func page(c *http.Client, address string, form url.Values, referer string) ([]byte, error) {
	return pageLimit(c, address, form, referer, maxPage)
}

func clean(s string, max int) string {
	s = strings.Join(strings.Fields(s), " ")
	if len(s) <= max {
		return s
	}
	for !utf8.RuneStart(s[max]) {
		max--
	}
	return s[:max]
}

func priceLabel(raw string) string {
	s := clean(raw, 26)
	if s == "" || s == "Free" {
		return "Free"
	}
	value := strings.Trim(s, "$€£¥ ")
	if n, err := strconv.ParseFloat(value, 64); err == nil && n == 0 {
		return "Free / name your price"
	}
	return s
}

func isFree(price string) bool {
	label := priceLabel(price)
	return label == "Free" || label == "Free / name your price"
}

type gameData struct {
	ID        int64  `json:"id"`
	Title     string `json:"title"`
	Price     string `json:"price"`
	Suggested string `json:"suggested_price"`
	Cover     string `json:"cover_image"`
}

func metadata(c *http.Client, base string) (gameData, error) {
	var data gameData
	raw, err := page(c, base+"/data.json", nil, "")
	if err != nil {
		return data, err
	}
	if json.Unmarshal(raw, &data) != nil || data.ID <= 0 || clean(data.Title, 95) == "" {
		return data, errVisit
	}
	return data, nil
}

type upload struct{ id, name string }

func uploads(doc []byte) []upload {
	root, err := html.Parse(bytes.NewReader(doc))
	if err != nil {
		return nil
	}
	var out []upload
	var walk func(*html.Node, *upload)
	walk = func(n *html.Node, item *upload) {
		var entry upload
		outer := n.Type == html.ElementNode && n.Data == "div" && nodeClass(n, "upload")
		if outer {
			item = &entry
		}
		if item != nil && n.Type == html.ElementNode {
			if n.Data == "a" {
				if id, err := strconv.ParseUint(nodeAttr(n, "data-upload_id"), 10, 64); err == nil && id != 0 {
					item.id = strconv.FormatUint(id, 10)
				}
			}
			if n.Data == "strong" && nodeClass(n, "name") {
				item.name = nodeAttr(n, "title")
				if item.name == "" && n.FirstChild != nil {
					item.name = strings.TrimSpace(n.FirstChild.Data)
				}
			}
		}
		for child := n.FirstChild; child != nil; child = child.NextSibling {
			walk(child, item)
		}
		if outer && entry.name != "" {
			out = append(out, entry)
		}
	}
	walk(root, nil)
	return out
}

func token(b []byte) string {
	if m := csrfTag.FindSubmatch(b); len(m) == 2 {
		return string(m[1])
	}
	return ""
}

func postURL(c *http.Client, address, referer string, values url.Values) (string, error) {
	var reply struct {
		URL string `json:"url"`
	}
	raw, err := page(c, address, values, referer)
	if err != nil {
		return "", err
	}
	if json.Unmarshal(raw, &reply) != nil || reply.URL == "" {
		return "", errVisit
	}
	u, err := url.Parse(reply.URL)
	if err != nil || !permitted(u) {
		return "", errVisit
	}
	return reply.URL, nil
}

func keyID(key string) string {
	payload, _, found := strings.Cut(key, ".")
	if !found {
		return ""
	}
	b, err := base64.RawStdEncoding.DecodeString(strings.TrimRight(payload, "="))
	if err != nil {
		b, err = base64.RawURLEncoding.DecodeString(strings.TrimRight(payload, "="))
	}
	var keyData struct {
		ID int64 `json:"id"`
	}
	if err != nil || json.Unmarshal(b, &keyData) != nil || keyData.ID <= 0 {
		return ""
	}
	return strconv.FormatInt(keyData.ID, 10)
}

func guestUploads(c *http.Client, base string) ([]upload, string, string, string, error) {
	// This page supplies a per-visit cookie and its CSRF token. A cached
	// listing page can omit the cookie even if it contains a token.
	session := fmt.Sprintf("%s?q2_playdate_session=%d", base, time.Now().UnixNano())
	doc, err := page(c, session, nil, base)
	if err != nil {
		return nil, "", "", "", err
	}
	csrf := token(doc)
	if csrf == "" {
		return nil, "", "", "", errVisit
	}
	signed, err := postURL(c, base+"/download_url", base,
		url.Values{"csrf_token": {csrf}, "suggested_amount": {"0"}})
	if err != nil {
		return nil, "", "", "", err
	}
	u, _ := url.Parse(signed)
	baseURL, _ := url.Parse(base)
	prefix := baseURL.EscapedPath() + "/download/"
	if u.Host != baseURL.Host || !strings.HasPrefix(u.EscapedPath(), prefix) {
		return nil, "", "", "", errVisit
	}
	key, err := url.PathUnescape(strings.TrimPrefix(u.EscapedPath(), prefix))
	if err != nil || key == "" {
		return nil, "", "", "", errVisit
	}
	doc, err = page(c, signed, nil, base)
	if err != nil {
		return nil, "", "", "", err
	}
	csrf = token(doc)
	if csrf == "" {
		return nil, "", "", "", errVisit
	}
	return uploads(doc), signed, csrf, keyID(key), nil
}

type catalogGame struct {
	id                                    int64
	title, creator, price, genre, address string
}

func nodeAttr(n *html.Node, name string) string {
	for _, attr := range n.Attr {
		if attr.Key == name {
			return attr.Val
		}
	}
	return ""
}

func nodeClass(n *html.Node, class string) bool {
	for _, part := range strings.Fields(nodeAttr(n, "class")) {
		if part == class {
			return true
		}
	}
	return false
}

func nodeText(n *html.Node) string {
	var text strings.Builder
	var walk func(*html.Node)
	walk = func(node *html.Node) {
		if node.Data == "svg" {
			return
		}
		if node.Type == html.TextNode {
			text.WriteString(node.Data)
		}
		for child := node.FirstChild; child != nil; child = child.NextSibling {
			walk(child)
		}
	}
	walk(n)
	return strings.TrimSpace(text.String())
}

func catalogEntries(raw []byte) ([]catalogGame, bool, error) {
	root, err := html.Parse(bytes.NewReader(raw))
	if err != nil {
		return nil, false, err
	}
	var entries []catalogGame
	seen := map[int64]bool{}
	more, grid := false, false
	var walk func(*html.Node, *catalogGame)
	walk = func(n *html.Node, game *catalogGame) {
		var entry catalogGame
		cell := n.Type == html.ElementNode && nodeClass(n, "game_cell")
		if cell {
			entry.id, _ = strconv.ParseInt(nodeAttr(n, "data-game_id"), 10, 64)
			game = &entry
		}
		if n.Type == html.ElementNode {
			if nodeClass(n, "browse_game_grid") {
				grid = true
			}
			if n.Data == "link" && nodeAttr(n, "rel") == "next" {
				more = true
			}
			if game != nil {
				switch {
				case n.Data == "a" && nodeClass(n, "title") && nodeClass(n, "game_link"):
					game.address = nodeAttr(n, "href")
					game.title = nodeText(n)
				case nodeClass(n, "game_author"):
					game.creator = nodeText(n)
				case nodeClass(n, "price_value"):
					game.price = nodeText(n)
				case nodeClass(n, "game_genre"):
					game.genre = nodeText(n)
				}
			}
		}
		for child := n.FirstChild; child != nil; child = child.NextSibling {
			walk(child, game)
		}
		if cell && entry.id > 0 && !seen[entry.id] && len(entries) < 80 {
			u, ok := project(entry.address)
			if ok && len(entry.address) < 250 && entry.title != "" {
				if entry.creator == "" {
					entry.creator = strings.TrimSuffix(u.Hostname(), ".itch.io")
				}
				entry.address = strings.TrimRight(entry.address, "/")
				seen[entry.id] = true
				entries = append(entries, entry)
			}
		}
	}
	walk(root, nil)
	// Itch uses an empty results message in place of the grid for an empty filter.
	if !grid && len(entries) == 0 && !bytes.Contains(raw, []byte("browse_game_grid")) &&
		!bytes.Contains(raw, []byte("No games")) && !bytes.Contains(raw, []byte("no_results")) &&
		!bytes.Contains(raw, []byte("0 results")) {
		return nil, false, errors.New("Could not read the itch.io catalog")
	}
	return entries, more, nil
}

func list(c *http.Client, number int, output io.Writer) error {
	if number < 1 || number > 100 {
		return errors.New("Invalid catalog page")
	}
	raw, err := page(c, fmt.Sprintf("https://itch.io/games/free/tag-playdate?page=%d", number), nil, "")
	if err != nil {
		return err
	}
	games, more, err := catalogEntries(raw)
	if err != nil {
		return err
	}
	fmt.Fprintf(output, "%t\n", more && number < 100)
	for _, game := range games {
		if isFree(game.price) {
			fmt.Fprintf(output, "%s\t%s\n", game.address, clean(game.title, 92))
		}
	}
	return nil
}
