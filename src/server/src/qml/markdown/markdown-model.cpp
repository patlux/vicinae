#include "markdown-model.hpp"
#include "font-service.hpp"
#include "image-url.hpp"
#include "syntax-highlighter.hpp"
#include "service-registry.hpp"
#include "services/app-service/app-service.hpp"
#include "theme/theme-file.hpp"
#include <QClipboard>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QUrlQuery>
#include <cmark-gfm.h>
#include <cmark-gfm-core-extensions.h>
#include <cmark-gfm-extension_api.h>
#include <pugixml/pugixml.hpp>
#include <cstring>
#include <utility>

namespace {

enum class GfmNodeType { Table, TableHeader, TableBody, TableRow, TableCell, Unknown };

GfmNodeType getGfmNodeType(cmark_node *node) {
  const char *t = cmark_node_get_type_string(node);
  if (!t) return GfmNodeType::Unknown;
  if (std::strcmp(t, "table") == 0) return GfmNodeType::Table;
  if (std::strcmp(t, "table_header") == 0) return GfmNodeType::TableHeader;
  if (std::strcmp(t, "table_body") == 0) return GfmNodeType::TableBody;
  if (std::strcmp(t, "table_row") == 0) return GfmNodeType::TableRow;
  if (std::strcmp(t, "table_cell") == 0) return GfmNodeType::TableCell;
  return GfmNodeType::Unknown;
}

QString imageProviderUrl(const QString &rawUrl) {
  QUrl const url(rawUrl);
  auto const scheme = url.scheme();

  if (scheme == "icon") return rawUrl;
  if (scheme == "https" || scheme == "http") return ImageURL::http(url).toString();

  if (scheme == "data") {
    ImageURL imgUrl;
    imgUrl.setType(ImageURLType::DataURI);
    imgUrl.setName(rawUrl);
    return imgUrl.toString();
  }

  if (scheme == "file") {
    auto path = url.host().isEmpty() ? url.path() : url.host() + url.path();
    return ImageURL::local(path).toString();
  }

  if (scheme.isEmpty()) return ImageURL::local(rawUrl).toString();

  return {};
}

struct InlineContext {
  const QString &inlineCodeFg;
  const QString &inlineCodeBg;
  const QString &linkColor;
  const QString &textColor;
  const QString &monoFamily;
};

QString imageAltText(cmark_node *imageNode) {
  QString alt;
  for (auto *c = cmark_node_first_child(imageNode); c; c = cmark_node_next(c)) {
    if (cmark_node_get_type(c) == CMARK_NODE_TEXT) {
      auto *lit = cmark_node_get_literal(c);
      if (lit) alt += QString::fromUtf8(lit);
    }
  }
  return alt;
}

QVariantMap parseImageSize(const QUrl &url) {
  QVariantMap data;
  QUrlQuery query(url);

  auto tryParam = [&](const std::vector<const char *> &names) -> int {
    for (auto *name : names) {
      auto val = query.queryItemValue(name);
      if (!val.isEmpty()) return val.toInt();
    }
    return 0;
  };

  int const w = tryParam({"raycast-width", "omnicast-width"});
  int const h = tryParam({"raycast-height", "omnicast-height"});
  if (w > 0) data[QStringLiteral("width")] = w;
  if (h > 0) data[QStringLiteral("height")] = h;
  return data;
}

QVariantMap parseDataUriSvgSize(const QString &rawUrl) {
  QVariantMap data;
  if (!rawUrl.startsWith(QStringLiteral("data:image/svg+xml"))) return data;

  const auto comma = rawUrl.indexOf(',');
  if (comma < 0) return data;

  QByteArray payload = rawUrl.mid(comma + 1).toUtf8();
  if (rawUrl.left(comma).contains(QStringLiteral(";base64"), Qt::CaseInsensitive))
    payload = QByteArray::fromBase64(payload);
  else
    payload = QByteArray::fromPercentEncoding(payload);

  // Only trust attributes on the root <svg> tag. Nested logo/rect widths would
  // otherwise collapse large account cards to tiny logo sizes.
  static const QRegularExpression svgOpenRe(QStringLiteral(R"((?is)<svg\b([^>]*)>)"));
  static const QRegularExpression attrRe(
      QStringLiteral(R"((?i)\b(width|height)\s*=\s*["']?(\d+(?:\.\d+)?))"));
  const auto open = svgOpenRe.match(QString::fromUtf8(payload));
  if (!open.hasMatch()) return data;

  auto it = attrRe.globalMatch(open.captured(1));
  while (it.hasNext()) {
    const auto match = it.next();
    const int value = qRound(match.captured(2).toDouble());
    if (value <= 0) continue;
    if (match.captured(1).compare(QStringLiteral("width"), Qt::CaseInsensitive) == 0)
      data[QStringLiteral("width")] = value;
    else
      data[QStringLiteral("height")] = value;
  }
  return data;
}

QString renderInlineHtml(cmark_node *node, const InlineContext &ctx);

QString renderOneInline(cmark_node *cur, const InlineContext &ctx) {
  QString result;

  switch (cmark_node_get_type(cur)) {
  case CMARK_NODE_TEXT:
    result += QString::fromUtf8(cmark_node_get_literal(cur)).toHtmlEscaped();
    break;

  case CMARK_NODE_SOFTBREAK:
    result += ' ';
    break;

  case CMARK_NODE_LINEBREAK:
    result += QStringLiteral("<br/>");
    break;

  case CMARK_NODE_CODE:
    result += QStringLiteral("<code style=\"color:%1;background:%2;"
                             "padding:1px 4px;border-radius:3px;"
                             "font-family:'%3',monospace;\">")
                  .arg(ctx.inlineCodeFg, ctx.inlineCodeBg, ctx.monoFamily);
    result += QString::fromUtf8(cmark_node_get_literal(cur)).toHtmlEscaped();
    result += QStringLiteral("</code>");
    break;

  case CMARK_NODE_STRONG:
    result += QStringLiteral("<b>");
    result += renderInlineHtml(cmark_node_first_child(cur), ctx);
    result += QStringLiteral("</b>");
    break;

  case CMARK_NODE_EMPH:
    result += QStringLiteral("<i>");
    result += renderInlineHtml(cmark_node_first_child(cur), ctx);
    result += QStringLiteral("</i>");
    break;

  case CMARK_NODE_LINK: {
    auto const href = QString::fromUtf8(cmark_node_get_url(cur));
    auto *child = cmark_node_first_child(cur);
    if (child && !cmark_node_next(child) && cmark_node_get_type(child) == CMARK_NODE_IMAGE &&
        href.startsWith('#')) {
      auto const rawUrl = QString::fromUtf8(cmark_node_get_url(child));
      // Qt RichText only understands normal image URLs (data:/http(s):/file:),
      // not Vicinae's internal icon: scheme used by ViciImage.
      auto const src = rawUrl;
      auto const alt = imageAltText(child);
      auto size = parseImageSize(QUrl(rawUrl));
      if (!size.contains(QStringLiteral("width")) || !size.contains(QStringLiteral("height"))) {
        auto svgSize = parseDataUriSvgSize(rawUrl);
        if (!size.contains(QStringLiteral("width")) && svgSize.contains(QStringLiteral("width")))
          size[QStringLiteral("width")] = svgSize.value(QStringLiteral("width"));
        if (!size.contains(QStringLiteral("height")) && svgSize.contains(QStringLiteral("height")))
          size[QStringLiteral("height")] = svgSize.value(QStringLiteral("height"));
      }
      // Compact account jump chips stay icon-sized and inline.
      int const width = size.value(QStringLiteral("width"), 34).toInt();
      int const height = size.value(QStringLiteral("height"), 34).toInt();
      result += QStringLiteral("<a href=\"%1\" style=\"color:%2;text-decoration:none;\">"
                               "<img src=\"%3\" alt=\"%4\" width=\"%5\" height=\"%6\"/>"
                               "</a>&nbsp;")
                    .arg(href.toHtmlEscaped(), ctx.linkColor, src.toHtmlEscaped(), alt.toHtmlEscaped())
                    .arg(width)
                    .arg(height);
      break;
    }

    result += QStringLiteral("<a href=\"%1\" style=\"color:%2;\">").arg(href.toHtmlEscaped(), ctx.linkColor);
    result += renderInlineHtml(child, ctx);
    result += QStringLiteral("</a>");
    break;
  }

  case CMARK_NODE_IMAGE: {
    auto const rawUrl = QString::fromUtf8(cmark_node_get_url(cur));
    // Keep data/http(s)/file URLs intact for RichText; convert only custom schemes.
    auto const scheme = QUrl(rawUrl).scheme();
    auto src = (scheme == QStringLiteral("data") || scheme == QStringLiteral("http") ||
                scheme == QStringLiteral("https") || scheme == QStringLiteral("file") || scheme.isEmpty())
                   ? rawUrl
                   : imageProviderUrl(rawUrl);
    auto alt = imageAltText(cur);
    auto size = parseImageSize(QUrl(rawUrl));
    if (!size.contains(QStringLiteral("width")) || !size.contains(QStringLiteral("height"))) {
      auto svgSize = parseDataUriSvgSize(rawUrl);
      if (!size.contains(QStringLiteral("width")) && svgSize.contains(QStringLiteral("width")))
        size[QStringLiteral("width")] = svgSize.value(QStringLiteral("width"));
      if (!size.contains(QStringLiteral("height")) && svgSize.contains(QStringLiteral("height")))
        size[QStringLiteral("height")] = svgSize.value(QStringLiteral("height"));
    }
    if (size.contains(QStringLiteral("width")) && size.contains(QStringLiteral("height"))) {
      result += QStringLiteral("<img src=\"%1\" alt=\"%2\" width=\"%3\" height=\"%4\"/>")
                    .arg(src.toHtmlEscaped(), alt.toHtmlEscaped())
                    .arg(size.value(QStringLiteral("width")).toInt())
                    .arg(size.value(QStringLiteral("height")).toInt());
    } else {
      result += QStringLiteral("<img src=\"%1\" alt=\"%2\"/>").arg(src.toHtmlEscaped(), alt.toHtmlEscaped());
    }
    break;
  }

  case CMARK_NODE_HTML_INLINE:
    result += QString::fromUtf8(cmark_node_get_literal(cur));
    break;

  default:
    if (std::strcmp(cmark_node_get_type_string(cur), "strikethrough") == 0) {
      result += QStringLiteral("<s>");
      result += renderInlineHtml(cmark_node_first_child(cur), ctx);
      result += QStringLiteral("</s>");
    } else {
      result += renderInlineHtml(cmark_node_first_child(cur), ctx);
    }
    break;
  }

  return result;
}

QString renderInlineHtml(cmark_node *node, const InlineContext &ctx) {
  QString result;
  for (auto *cur = node; cur; cur = cmark_node_next(cur))
    result += renderOneInline(cur, ctx);
  return result;
}

QString renderInlineChildren(cmark_node *parent, const InlineContext &ctx) {
  return renderInlineHtml(cmark_node_first_child(parent), ctx);
}

QVariantMap buildListItem(cmark_node *itemNode, const InlineContext &ctx);

QVariantList buildListItems(cmark_node *listNode, const InlineContext &ctx) {
  QVariantList items;
  for (auto *item = cmark_node_first_child(listNode); item; item = cmark_node_next(item)) {
    if (cmark_node_get_type(item) == CMARK_NODE_ITEM) items.append(buildListItem(item, ctx));
  }
  return items;
}

QVariantMap buildListItem(cmark_node *itemNode, const InlineContext &ctx) {
  QVariantMap entry;
  QVariantList children;

  for (auto *child = cmark_node_first_child(itemNode); child; child = cmark_node_next(child)) {
    auto type = cmark_node_get_type(child);
    if (type == CMARK_NODE_PARAGRAPH) {
      entry[QStringLiteral("html")] = renderInlineChildren(child, ctx);
    } else if (type == CMARK_NODE_LIST) {
      QVariantMap sub;
      sub[QStringLiteral("ordered")] = (cmark_node_get_list_type(child) == CMARK_ORDERED_LIST);
      sub[QStringLiteral("startNumber")] = cmark_node_get_list_start(child);
      sub[QStringLiteral("items")] = buildListItems(child, ctx);
      children.append(sub);
    }
  }

  entry[QStringLiteral("children")] = children;
  return entry;
}

void collectCellHtml(cmark_node *firstCell, int columnCount, QVariantList &out, const InlineContext &ctx) {
  int c = 0;
  for (auto *cell = firstCell; cell && c < columnCount; cell = cmark_node_next(cell)) {
    if (getGfmNodeType(cell) != GfmNodeType::TableCell) continue;
    QVariantMap cellData;
    cellData[QStringLiteral("html")] = renderInlineChildren(cell, ctx);
    out.append(cellData);
    ++c;
  }
}

QVariantMap buildImageBlock(cmark_node *imageNode) {
  QString const rawUrl = QString::fromUtf8(cmark_node_get_url(imageNode));
  QUrl const imgUrl(rawUrl);
  QVariantMap data = parseImageSize(imgUrl);
  if (!data.contains(QStringLiteral("width")) || !data.contains(QStringLiteral("height"))) {
    auto svgSize = parseDataUriSvgSize(rawUrl);
    if (!data.contains(QStringLiteral("width")) && svgSize.contains(QStringLiteral("width")))
      data[QStringLiteral("width")] = svgSize.value(QStringLiteral("width"));
    if (!data.contains(QStringLiteral("height")) && svgSize.contains(QStringLiteral("height")))
      data[QStringLiteral("height")] = svgSize.value(QStringLiteral("height"));
  }
  data[QStringLiteral("src")] = imageProviderUrl(rawUrl);
  data[QStringLiteral("alt")] = imageAltText(imageNode);
  return data;
}

struct HtmlBlockResult {
  QString html;
  std::vector<QVariantMap> extractedImages;
};

void processHtmlNodes(pugi::xml_node node, HtmlBlockResult &result) {
  for (auto cur = node.first_child(); cur; cur = cur.next_sibling()) {
    if (cur.type() == pugi::node_element) {
      if (std::strcmp(cur.name(), "img") == 0) {
        QString src;
        QString alt;
        int w = 0, h = 0;
        for (auto attr = cur.first_attribute(); attr; attr = attr.next_attribute()) {
          QString const name = QString::fromUtf8(attr.name()).toLower();
          QString const value = QString::fromUtf8(attr.value());

          if (name == "src") {
            src = value;
          } else if (name == "alt") {
            alt = value;
          } else if (name == "width") {
            w = value.toInt();
          } else if (name == "height") {
            h = value.toInt();
          } else if (name == "style") {
            for (const auto &decl : value.split(';', Qt::SkipEmptyParts)) {
              auto parts = decl.split(':', Qt::SkipEmptyParts);
              if (parts.size() != 2) continue;
              QString const prop = parts[0].trimmed().toLower();
              QString pval = parts[1].trimmed();
              if (pval.endsWith("px", Qt::CaseInsensitive)) pval.chop(2);
              if (prop == "width")
                w = pval.trimmed().toInt();
              else if (prop == "height")
                h = pval.trimmed().toInt();
            }
          }
        }
        if (!src.isEmpty()) {
          if (w <= 0 || h <= 0) {
            auto svgSize = parseDataUriSvgSize(src);
            if (w <= 0 && svgSize.contains(QStringLiteral("width")))
              w = svgSize.value(QStringLiteral("width")).toInt();
            if (h <= 0 && svgSize.contains(QStringLiteral("height")))
              h = svgSize.value(QStringLiteral("height")).toInt();
          }
          QVariantMap img;
          img[QStringLiteral("src")] = imageProviderUrl(src);
          img[QStringLiteral("alt")] = alt;
          if (w > 0) img[QStringLiteral("width")] = w;
          if (h > 0) img[QStringLiteral("height")] = h;
          result.extractedImages.push_back(img);
        }
      } else {
        processHtmlNodes(cur, result);
      }
    } else if (cur.type() == pugi::node_pcdata) {
      QString const text = QString::fromUtf8(cur.value()).trimmed();
      if (!text.isEmpty()) result.html += text.toHtmlEscaped();
    }
  }
}

} // anonymous namespace

MarkdownModel::MarkdownModel(QObject *parent) : QAbstractListModel(parent) {
  rebuildInlineStyles();
  connect(&ThemeService::instance(), &ThemeService::themeChanged, this, [this]() {
    rebuildInlineStyles();
    if (!m_markdown.isEmpty()) setMarkdown(m_markdown);
  });
}

int MarkdownModel::rowCount(const QModelIndex &) const { return static_cast<int>(m_blocks.size()); }

QVariant MarkdownModel::data(const QModelIndex &index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_blocks.size())) return {};

  const auto &block = m_blocks[index.row()];
  switch (role) {
  case BlockTypeRole:
    return static_cast<int>(block.type);
  case BlockDataRole:
    return block.data;
  default:
    return {};
  }
}

QHash<int, QByteArray> MarkdownModel::roleNames() const {
  return {
      {BlockTypeRole, "blockType"},
      {BlockDataRole, "blockData"},
  };
}

void MarkdownModel::rebuildInlineStyles() {
  auto &theme = ThemeService::instance().theme();
  m_inlineCodeFg = theme.resolve(SemanticColor::Foreground).name(QColor::HexRgb);
  m_inlineCodeBg = theme.resolve(SemanticColor::SecondaryBackground).name(QColor::HexRgb);
  m_linkColor = theme.resolve(SemanticColor::LinkDefault).name(QColor::HexRgb);
  m_textColor = theme.resolve(SemanticColor::Foreground).name(QColor::HexRgb);
  m_monoFamily = ServiceRegistry::instance()->fontService()->builtinMonoFontFamily();
  m_syntaxStyles = syntax::buildStyleMap(theme);
}

std::vector<MarkdownModel::Block> MarkdownModel::parseBlocks(const QString &markdown) const {
  std::vector<Block> blocks;
  blocks.reserve(m_blocks.size() + 4);

  auto buf = markdown.toUtf8();

  cmark_gfm_core_extensions_ensure_registered();
  cmark_parser *parser = cmark_parser_new(CMARK_OPT_DEFAULT);

  if (auto *tableExt = cmark_find_syntax_extension("table"))
    cmark_parser_attach_syntax_extension(parser, tableExt);
  if (auto *strikethroughExt = cmark_find_syntax_extension("strikethrough"))
    cmark_parser_attach_syntax_extension(parser, strikethroughExt);

  cmark_parser_feed(parser, buf.data(), buf.size());
  cmark_node *root = cmark_parser_finish(parser);
  cmark_parser_free(parser);

  InlineContext ctx{m_inlineCodeFg, m_inlineCodeBg, m_linkColor, m_textColor, m_monoFamily};

  for (auto *node = cmark_node_first_child(root); node; node = cmark_node_next(node)) {
    auto type = cmark_node_get_type(node);

    switch (type) {
    case CMARK_NODE_PARAGRAPH: {
      bool hasImage = false;
      bool internalImageLinksOnly = cmark_node_first_child(node) != nullptr;
      for (auto *c = cmark_node_first_child(node); c; c = cmark_node_next(c)) {
        auto ct = cmark_node_get_type(c);
        if (ct == CMARK_NODE_SOFTBREAK || ct == CMARK_NODE_LINEBREAK) continue;
        if (ct == CMARK_NODE_TEXT) {
          if (QString::fromUtf8(cmark_node_get_literal(c)).trimmed().isEmpty()) continue;
          internalImageLinksOnly = false;
          continue;
        }
        if (ct == CMARK_NODE_IMAGE) {
          hasImage = true;
          internalImageLinksOnly = false;
          break;
        }
        if (ct == CMARK_NODE_LINK) {
          auto *lc = cmark_node_first_child(c);
          if (lc && !cmark_node_next(lc) && cmark_node_get_type(lc) == CMARK_NODE_IMAGE) {
            hasImage = true;
            if (!QString::fromUtf8(cmark_node_get_url(c)).startsWith('#')) internalImageLinksOnly = false;
            continue;
          }
        }
        internalImageLinksOnly = false;
      }

      if (!hasImage) {
        QVariantMap data;
        data[QStringLiteral("html")] = renderInlineChildren(node, ctx);
        blocks.push_back({MdBlockType::Paragraph, data});
        break;
      }

      // Pure internal image-link rows (account jump chips) become real image
      // blocks so TapHandler can open the fragment and scroll reliably.
      if (internalImageLinksOnly) {
        QVariantList images;
        for (auto *c = cmark_node_first_child(node); c; c = cmark_node_next(c)) {
          if (cmark_node_get_type(c) != CMARK_NODE_LINK) continue;
          auto *lc = cmark_node_first_child(c);
          if (!lc || cmark_node_next(lc) || cmark_node_get_type(lc) != CMARK_NODE_IMAGE) continue;
          auto data = buildImageBlock(lc);
          data[QStringLiteral("link")] = QString::fromUtf8(cmark_node_get_url(c));
          images.append(data);
        }
        if (!images.isEmpty()) {
          QVariantMap row;
          row[QStringLiteral("images")] = images;
          blocks.push_back({MdBlockType::ImageRow, row});
          break;
        }
      }

      auto flushRun = [&blocks](QString &run) {
        if (run.trimmed().isEmpty()) {
          run.clear();
          return;
        }
        QVariantMap data;
        data[QStringLiteral("html")] = run;
        blocks.push_back({MdBlockType::Paragraph, data});
        run.clear();
      };

      QString run;
      for (auto *c = cmark_node_first_child(node); c; c = cmark_node_next(c)) {
        auto ct = cmark_node_get_type(c);

        if (ct == CMARK_NODE_IMAGE) {
          flushRun(run);
          blocks.push_back({MdBlockType::Image, buildImageBlock(c)});
          continue;
        }

        if (ct == CMARK_NODE_LINK) {
          auto *lc = cmark_node_first_child(c);
          if (lc && !cmark_node_next(lc) && cmark_node_get_type(lc) == CMARK_NODE_IMAGE) {
            flushRun(run);
            auto data = buildImageBlock(lc);
            data[QStringLiteral("link")] = QString::fromUtf8(cmark_node_get_url(c));
            blocks.push_back({MdBlockType::Image, data});
            continue;
          }
        }

        run += renderOneInline(c, ctx);
      }
      flushRun(run);
      break;
    }

    case CMARK_NODE_HEADING: {
      QVariantMap data;
      data[QStringLiteral("level")] = cmark_node_get_heading_level(node);
      data[QStringLiteral("html")] = renderInlineChildren(node, ctx);
      blocks.push_back({MdBlockType::Heading, data});
      break;
    }

    case CMARK_NODE_CODE_BLOCK: {
      QVariantMap data;
      QString code = QString::fromUtf8(cmark_node_get_literal(node));
      while (!code.isEmpty() && code.back().isSpace())
        code.chop(1);
      data[QStringLiteral("code")] = code;
      auto *lang = cmark_node_get_fence_info(node);
      QString const language = lang ? QString::fromUtf8(lang) : QString();
      data[QStringLiteral("language")] = language;
      bool const isDark = ThemeService::instance().theme().isDark();
      data[QStringLiteral("highlightedHtml")] = syntax::highlight(code, language, m_syntaxStyles, isDark);
      blocks.push_back({MdBlockType::CodeBlock, data});
      break;
    }

    case CMARK_NODE_LIST: {
      bool const ordered = (cmark_node_get_list_type(node) == CMARK_ORDERED_LIST);
      QVariantMap data;
      data[QStringLiteral("items")] = buildListItems(node, ctx);
      if (ordered) {
        data[QStringLiteral("startNumber")] = cmark_node_get_list_start(node);
        blocks.push_back({MdBlockType::OrderedList, data});
      } else {
        blocks.push_back({MdBlockType::BulletList, data});
      }
      break;
    }

    case CMARK_NODE_THEMATIC_BREAK:
      blocks.push_back({MdBlockType::HorizontalRule, {}});
      break;

    case CMARK_NODE_HTML_BLOCK: {
      QString const html = QString::fromUtf8(cmark_node_get_literal(node));
      QByteArray const wrapped = "<div>" + html.toUtf8() + "</div>";

      pugi::xml_document doc;
      auto parseResult = doc.load_buffer(wrapped.constData(), wrapped.size(),
                                         pugi::parse_default | pugi::parse_ws_pcdata_single);
      if (parseResult) {
        auto root = doc.first_child();
        HtmlBlockResult result;
        processHtmlNodes(root, result);

        for (auto &img : result.extractedImages)
          blocks.push_back({MdBlockType::Image, img});

        if (!result.html.isEmpty()) {
          QVariantMap data;
          data[QStringLiteral("html")] = result.html;
          blocks.push_back({MdBlockType::HtmlBlock, data});
        }
      } else {
        QVariantMap data;
        data[QStringLiteral("html")] = html;
        blocks.push_back({MdBlockType::HtmlBlock, data});
      }
      break;
    }

    case CMARK_NODE_BLOCK_QUOTE: {
      auto *firstChild = cmark_node_first_child(node);
      QString calloutType;

      if (firstChild && cmark_node_get_type(firstChild) == CMARK_NODE_PARAGRAPH) {
        auto *textNode = cmark_node_first_child(firstChild);
        if (textNode && cmark_node_get_type(textNode) == CMARK_NODE_TEXT) {
          QString const text = QString::fromUtf8(cmark_node_get_literal(textNode));
          static const QRegularExpression calloutRe(
              QStringLiteral("^\\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\\]\\s*"));
          auto match = calloutRe.match(text);
          if (match.hasMatch()) {
            calloutType = match.captured(1).toLower();
            QString const remaining = text.mid(match.capturedLength());
            cmark_node_set_literal(textNode, remaining.toUtf8().constData());
          }
        }
      }

      QVariantList paragraphs;
      for (auto *child = cmark_node_first_child(node); child; child = cmark_node_next(child)) {
        if (cmark_node_get_type(child) == CMARK_NODE_PARAGRAPH) {
          QString const html = renderInlineChildren(child, ctx);
          if (!html.isEmpty()) paragraphs.append(html);
        }
      }

      QVariantMap data;
      data[QStringLiteral("paragraphs")] = paragraphs;

      if (!calloutType.isEmpty()) {
        data[QStringLiteral("calloutType")] = calloutType;
        blocks.push_back({MdBlockType::Callout, data});
      } else {
        blocks.push_back({MdBlockType::Blockquote, data});
      }
      break;
    }

    default:
      if (getGfmNodeType(node) == GfmNodeType::Table) {
        int columnCount = cmark_gfm_extensions_get_table_columns(node);
        if (columnCount <= 0) break;

        QVariantMap data;
        data[QStringLiteral("columnCount")] = columnCount;

        QVariantList alignments;
        if (auto *aligns = cmark_gfm_extensions_get_table_alignments(node)) {
          for (int c = 0; c < columnCount; ++c) {
            switch (aligns[c]) {
            case 'c':
              alignments.append(1);
              break;
            case 'r':
              alignments.append(2);
              break;
            default:
              alignments.append(0);
              break;
            }
          }
        } else {
          for (int c = 0; c < columnCount; ++c)
            alignments.append(0);
        }
        data[QStringLiteral("alignments")] = alignments;

        cmark_node *header = nullptr;
        cmark_node *body = nullptr;
        std::vector<cmark_node *> extraRows;

        for (auto *child = cmark_node_first_child(node); child; child = cmark_node_next(child)) {
          switch (getGfmNodeType(child)) {
          case GfmNodeType::TableHeader:
            header = child;
            break;
          case GfmNodeType::TableBody:
            body = child;
            break;
          case GfmNodeType::TableRow:
            extraRows.push_back(child);
            break;
          default:
            break;
          }
        }

        QVariantList headers;
        if (header) {
          auto *first = cmark_node_first_child(header);
          if (first && getGfmNodeType(first) == GfmNodeType::TableCell) {
            collectCellHtml(first, columnCount, headers, ctx);
          } else if (first && getGfmNodeType(first) == GfmNodeType::TableRow) {
            collectCellHtml(cmark_node_first_child(first), columnCount, headers, ctx);
          }
        }
        data[QStringLiteral("headers")] = headers;

        QVariantList rows;
        auto addRowsFromSection = [&](cmark_node *section) {
          if (!section) return;
          auto *first = cmark_node_first_child(section);
          if (!first) return;
          if (getGfmNodeType(first) == GfmNodeType::TableCell) {
            QVariantList row;
            collectCellHtml(first, columnCount, row, ctx);
            rows.append(QVariant(row));
          } else {
            for (auto *r = first; r; r = cmark_node_next(r)) {
              if (getGfmNodeType(r) != GfmNodeType::TableRow) continue;
              QVariantList row;
              collectCellHtml(cmark_node_first_child(r), columnCount, row, ctx);
              rows.append(QVariant(row));
            }
          }
        };
        addRowsFromSection(body);
        for (auto *row : extraRows) {
          QVariantList rowData;
          collectCellHtml(cmark_node_first_child(row), columnCount, rowData, ctx);
          rows.append(QVariant(rowData));
        }
        data[QStringLiteral("rows")] = rows;

        blocks.push_back({MdBlockType::Table, data});
      }
      break;
    }
  }

  cmark_node_free(root);
  return blocks;
}

void MarkdownModel::setMarkdown(const QString &markdown) {
  // Incremental append: new content is a strict prefix-extension of old
  if (!m_blocks.empty() && !m_markdown.isEmpty() && markdown.size() > m_markdown.size() &&
      markdown.startsWith(m_markdown)) {
    m_markdown = markdown;
    auto newBlocks = parseBlocks(m_markdown);
    auto oldCount = static_cast<int>(m_blocks.size());
    auto newCount = static_cast<int>(newBlocks.size());

    if (newCount >= oldCount) {
      int divergeAt = oldCount;
      if (oldCount > 0) {
        auto &lastOld = m_blocks[oldCount - 1];
        auto &lastNew = newBlocks[oldCount - 1];
        if (lastOld.type != lastNew.type || lastOld.data != lastNew.data) divergeAt = oldCount - 1;
      }

      if (divergeAt < oldCount) {
        beginRemoveRows({}, divergeAt, oldCount - 1);
        m_blocks.erase(m_blocks.begin() + divergeAt, m_blocks.end());
        endRemoveRows();
      }

      if (divergeAt < newCount) {
        beginInsertRows({}, divergeAt, newCount - 1);
        m_blocks.insert(m_blocks.end(), std::make_move_iterator(newBlocks.begin() + divergeAt),
                        std::make_move_iterator(newBlocks.end()));
        endInsertRows();
      }

      emit blocksAppended();
      return;
    }
  }

  m_markdown = markdown;
  beginResetModel();
  m_blocks.clear();

  if (!markdown.isEmpty()) m_blocks = parseBlocks(markdown);

  endResetModel();
}

void MarkdownModel::clear() {
  beginResetModel();
  m_blocks.clear();
  m_markdown.clear();
  endResetModel();
}

void MarkdownModel::openLink(const QString &url) {
  if (url.startsWith('#')) {
    // TextEdit may leave the fragment percent-encoded or already decoded.
    // Normalize both so plugin anchors like "claude:auth" still match.
    auto target = QUrl::fromPercentEncoding(url.sliced(1).trimmed().toUtf8()).trimmed();
    if (!target.isEmpty()) emit internalLinkRequested(target);
    return;
  }

  auto *appDb = ServiceRegistry::instance()->appDb();
  if (!appDb->openTarget(url)) qWarning() << "MarkdownModel: failed to open link" << url;
}

QString MarkdownModel::copyCodeBlock(int blockIndex) {
  if (blockIndex < 0 || std::cmp_greater_equal(blockIndex, m_blocks.size())) return {};
  const auto &block = m_blocks[blockIndex];
  if (block.type != MdBlockType::CodeBlock) return {};
  auto code = block.data.value(QStringLiteral("code")).toString();
  QGuiApplication::clipboard()->setText(code);
  return code;
}
