/** The history drawer: every saved conversation, newest first. */
import type { JSX } from 'react'
import type { SessionMeta } from '@shared/store'

interface Props {
  sessions: SessionMeta[]
  currentId: string
  onSelect: (id: string) => void
  onNew: () => void
  /** A turn is running; a new conversation would leave it streaming into nothing. */
  streaming: boolean
  onDelete: (id: string) => void
  onOpenSettings: () => void
}

export function Sidebar({
  sessions,
  currentId,
  onSelect,
  onNew,
  streaming,
  onDelete,
  onOpenSettings
}: Props): JSX.Element {
  return (
    <aside className="sidebar">
      <div className="sidebar-head">
        <span className="wordmark">YoAI</span>
        <button className="btn btn-quiet" onClick={onNew} disabled={streaming} title="新对话">
          ＋
        </button>
      </div>

      <div className="session-list">
        {sessions.length === 0 ? (
          <p className="sidebar-empty">还没有对话</p>
        ) : (
          sessions.map((s) => (
            <div key={s.id} className="session-row" aria-current={s.id === currentId}>
              <button className="session-title" onClick={() => onSelect(s.id)} title={s.title}>
                {s.title.length > 0 ? s.title : '（空）'}
              </button>
              <span className="session-count">{s.count}</span>
              <button className="session-drop" title="删除这个对话" onClick={() => onDelete(s.id)}>
                ×
              </button>
            </div>
          ))
        )}
      </div>

      <div className="sidebar-foot">
        <button className="btn btn-quiet sidebar-settings" onClick={onOpenSettings}>
          设置
        </button>
      </div>
    </aside>
  )
}
