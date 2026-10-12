/**
 * An assistant reply, rendered from the markdown it is written in.
 *
 * Models write lists, tables and code as markdown, and shown as plain text a
 * reply read as a page of dashes, pipes and fences. Only the reply is rendered:
 * a user's message is what they typed, and it is shown as typed.
 *
 * What is deliberately NOT done:
 *
 *   - **No raw HTML.** react-markdown shows it as text unless a plugin turns it
 *     on, and none is. A reply is text from a model -- or from whatever the
 *     model was shown -- and a `<script>` in it must stay words.
 *   - **No images.** Loading one fetches a URL the reply chose, which is how a
 *     page is made to report that it was read. The alt text is shown instead.
 *   - **No navigation inside the window.** A link opens in the system browser,
 *     through the main process, which accepts http and https only.
 *
 * Re-parsed on every delta while a reply streams. That is cheap at the length
 * of a reply; what it costs is a marker not yet closed -- `**粗` before its `**`
 * arrives -- showing as written for a moment.
 */
import { memo, type JSX } from 'react'
import ReactMarkdown, { type Components } from 'react-markdown'
import remarkGfm from 'remark-gfm'

const PLUGINS = [remarkGfm]

const COMPONENTS: Components = {
  a: ({ href, children }) => (
    <a
      href={href}
      onClick={(e) => {
        e.preventDefault()
        if (href !== undefined && href.length > 0) {
          // A relative or odd link is refused by the main process; nothing to
          // do about it here but not throw.
          window.yoai.openExternal(href).catch(() => undefined)
        }
      }}
    >
      {children}
    </a>
  ),
  img: ({ alt }) => <span className="md-img">{alt !== undefined && alt.length > 0 ? alt : '图片'}</span>,
  // Wide tables scroll inside the bubble rather than widening it.
  table: ({ children }) => (
    <div className="md-table">
      <table>{children}</table>
    </div>
  )
}

export const Markdown = memo(function Markdown({ text }: { text: string }): JSX.Element {
  return (
    <div className="md">
      <ReactMarkdown remarkPlugins={PLUGINS} components={COMPONENTS}>
        {text}
      </ReactMarkdown>
    </div>
  )
})
