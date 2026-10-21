package app.localarchiver

import android.Manifest
import android.app.*
import android.content.*
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.*
import android.provider.Settings
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.text.method.PasswordTransformationMethod
import android.view.*
import android.widget.*
import androidx.core.content.FileProvider
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import java.io.File
import java.text.DateFormat
import java.util.Date
import java.util.concurrent.Executors

class MainActivity : Activity() {
    private lateinit var storage: Storage
    private val io = Executors.newSingleThreadExecutor()
    private val prefs by lazy { getSharedPreferences("mobile-settings", MODE_PRIVATE) }
    private lateinit var root: LinearLayout
    private lateinit var body: FrameLayout
    private lateinit var title: TextView
    private lateinit var subtitle: TextView
    private lateinit var mini: TextView
    private lateinit var nav: LinearLayout
    private var tab = 0
    private var folder: Node? = null
    private val parents = mutableListOf<Node>()
    private var archive: ArchivePage? = null
    private var pageVersion = 0
    private var files = emptyList<Node>()
    private var visible = emptyList<Node>()
    private val selected = linkedSetOf<String>()
    private var query = ""
    private var sort = "name"
    private var epoch = 0
    private var resumed = false
    private var promptId: String? = null
    private var promptDialog: AlertDialog? = null
    private var activeText: TextView? = null
    private var activeProgress: ProgressBar? = null
    private var latestRecord: String? = null
    private var passwordDraft = mutableListOf<String>()
    private var passwordDirty = false
    private var revealPasswords = false
    private var pendingExtract: Node? = null
    private var pendingSiblings = emptyList<Node>()
    private var pendingSettings = ExtractSettings()
    private var pendingName = ""
    private var externalSources = emptyList<Node>()
    private var loading = false
    private data class Retained(val parents: List<Node>, val archive: ArchivePage?, val draft: List<String>, val dirty: Boolean, val query: String, val selected: Set<String>)
    override fun onRetainNonConfigurationInstance(): Any = Retained(parents.toList(), archive, passwordDraft.toList(), passwordDirty, query, selected.toSet())
    private val dark get() = resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK == Configuration.UI_MODE_NIGHT_YES
    private val bg get() = Color.parseColor(if (dark) "#141B18" else "#F7F9F6")
    private val surface get() = Color.parseColor(if (dark) "#202A24" else "#FFFFFF")
    private val ink get() = Color.parseColor(if (dark) "#E2EBE3" else "#202D25")
    private val muted get() = Color.parseColor(if (dark) "#A0B0A5" else "#68776C")
    private val green get() = Color.parseColor(if (dark) "#A1D7BD" else "#397461")
    private val tint get() = Color.parseColor(if (dark) "#293E32" else "#E5F0E7")
    private val observer: () -> Unit = { updateTask() }
    private fun dp(n: Int) = (n * resources.displayMetrics.density).toInt()
    private fun text(value: String, size: Float = 14f, color: Int = ink) = TextView(this).apply { text = value; textSize = size; setTextColor(color); gravity = Gravity.CENTER_VERTICAL }
    private fun column() = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
    private fun row() = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
    private fun rounded(color: Int, radius: Int = 16) = GradientDrawable().apply { setColor(color); cornerRadius = dp(radius).toFloat() }
    private fun button(label: String, primary: Boolean = false, click: () -> Unit) = Button(this).apply {
        text = label; isAllCaps = false; textSize = 14f; minHeight = dp(48); minimumWidth = dp(48)
        setTextColor(if (primary) if (dark) Color.parseColor("#173627") else Color.WHITE else green)
        background = rounded(if (primary) green else tint, 14)
        setPadding(dp(12), dp(6), dp(12), dp(6)); setOnClickListener { click() }
    }
    private fun LinearLayout.add(view: View, weight: Float = 0f) { addView(view, LinearLayout.LayoutParams(if (weight > 0) 0 else -1, -2, weight).apply { if (orientation == LinearLayout.HORIZONTAL) { marginEnd = dp(6); width = if (weight > 0) 0 else -2 } else bottomMargin = dp(8) }) }
    private fun padded(view: LinearLayout, amount: Int = 16) = view.apply { setPadding(dp(amount), dp(amount), dp(amount), dp(amount)) }
    private fun notify(value: String) { Toast.makeText(this, value, Toast.LENGTH_LONG).show() }
    private fun failure(error: Throwable) { notify(error.message ?: "操作失败，请重试") }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        storage = Storage(this); Tasks.init(this)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        sort = prefs.getString("sort", "name") ?: "name"
        tab = savedInstanceState?.getInt("tab") ?: if (intent.getBooleanExtra("tasks", false)) 1 else 0
        savedInstanceState?.getString("folder")?.let { location ->
            folder = if (location.startsWith("content:")) runCatching { storage.fromUri(Uri.parse(location), savedInstanceState.getString("tree")) }.getOrNull() else storage.fromFile(File(location))
        }
        if (folder == null) folder = storage.fromFile(if (storage.allFiles) File(storage.shared, "Download").takeIf { it.isDirectory } ?: storage.shared else storage.results)
        archive = Tasks.page
        val retained = lastNonConfigurationInstance as? Retained
        retained?.let { parents.addAll(it.parents); archive = it.archive; passwordDraft = it.draft.toMutableList(); passwordDirty = it.dirty; query = it.query; selected.addAll(it.selected) }
        pageVersion = Tasks.pageVersion
        latestRecord = Tasks.records.firstOrNull()?.id
        buildShell()
        if (tab == 2 && retained == null) loadPasswords() else render()
        if (savedInstanceState == null) handleIntent(intent)
    }
    override fun onSaveInstanceState(outState: Bundle) {
        outState.putInt("tab", tab); outState.putString("folder", folder?.location); outState.putString("tree", folder?.tree)
        super.onSaveInstanceState(outState)
    }
    override fun onNewIntent(intent: Intent) { super.onNewIntent(intent); setIntent(intent); if (intent.getBooleanExtra("tasks", false)) switchTab(1) else handleIntent(intent) }
    override fun onResume() { super.onResume(); resumed = true; Tasks.listeners.add(observer); updateTask() }
    override fun onPause() { resumed = false; Tasks.listeners.remove(observer); super.onPause() }
    override fun onDestroy() { promptDialog?.dismiss(); io.shutdown(); super.onDestroy() }
    private fun buildShell() {
        root = column().apply { setBackgroundColor(bg) }
        ViewCompat.setOnApplyWindowInsetsListener(root) { view, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom); insets
        }
        val header = row().apply { setPadding(dp(20), dp(16), dp(12), dp(8)) }
        val labels = column(); title = text("MilkZip", 27f).apply { setTypeface(null, Typeface.BOLD) }
        subtitle = text("轻松解压，一层到底", 12f, muted)
        labels.addView(title); labels.addView(subtitle)
        header.addView(labels, LinearLayout.LayoutParams(0, -2, 1f))
        header.addView(button("＋") { mainMenu() }, LinearLayout.LayoutParams(dp(48), dp(48)))
        root.addView(header)
        body = FrameLayout(this); root.addView(body, LinearLayout.LayoutParams(-1, 0, 1f))
        mini = text("", 13f, green).apply { setPadding(dp(20), dp(12), dp(20), dp(12)); background = rounded(tint, 0); setOnClickListener { switchTab(1) } }
        root.addView(mini)
        nav = row().apply { setPadding(dp(12), dp(8), dp(6), dp(8)); setBackgroundColor(surface) }
        root.addView(nav); setContentView(root)
        WindowCompat.getInsetsController(window, root).isAppearanceLightNavigationBars = !dark
        if (Build.VERSION.SDK_INT == 26) window.navigationBarColor = Color.parseColor("#397461")
        if (Build.VERSION.SDK_INT >= 33) onBackInvokedDispatcher.registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT) { onBackPressed() }
        renderNav()
    }
    private fun renderNav() {
        nav.removeAllViews()
        listOf("文件", "任务", "密码本").forEachIndexed { index, label ->
            val v = text(label, 14f, if (tab == index) green else muted).apply {
                gravity = Gravity.CENTER; minHeight = dp(52); background = rounded(if (tab == index) tint else surface, 16)
                if (tab == index) setTypeface(null, Typeface.BOLD)
                tag = "nav$index"; contentDescription = label; setOnClickListener { switchTab(index) }
            }
            nav.addView(v, LinearLayout.LayoutParams(0, dp(52), 1f).apply { marginEnd = dp(6) })
        }
    }
    private fun switchTab(value: Int) {
        if (value == tab) return
        if (tab == 2 && passwordDirty) {
            AlertDialog.Builder(this).setTitle("密码本有未保存的修改").setPositiveButton("保存") { _, _ -> if (savePasswords()) { tab = value; render() } }
                .setNegativeButton("放弃修改") { _, _ -> passwordDirty = false; tab = value; render() }.setNeutralButton("继续编辑", null).show(); return
        }
        tab = value; selected.clear()
        if (tab == 2) loadPasswords() else render()
    }
    private fun render() {
        if (tab == 2) window.addFlags(WindowManager.LayoutParams.FLAG_SECURE) else if (prompt == null) window.clearFlags(WindowManager.LayoutParams.FLAG_SECURE)
        renderNav(); body.removeAllViews(); activeText = null; activeProgress = null
        title.text = when (tab) { 1 -> "任务"; 2 -> "密码本"; else -> "MilkZip" }
        subtitle.text = when (tab) { 1 -> "进度、结果和处理报告"; 2 -> "仅保存在这台设备，按顺序尝试"; else -> "轻松解压，一层到底" }
        when (tab) { 0 -> renderFiles(); 1 -> renderTasks(); 2 -> renderPasswords() }
        updateTaskViews()
    }
    private val prompt get() = Tasks.prompt
    private fun renderFiles() {
        val layout = column(); body.addView(layout)
        val quick = row().apply { setPadding(dp(16), dp(6), dp(10), dp(6)) }
        quick.add(button("手机存储") { if (storage.allFiles) navigate(storage.fromFile(storage.shared)) else storageAccess() }, 1f)
        quick.add(button("结果") { navigate(storage.fromFile(storage.results)) }, 1f)
        quick.add(button("选文件") { pickFiles() }, 1f)
        layout.addView(quick)
        val crumbs = row().apply { setPadding(dp(16), dp(4), dp(16), dp(6)) }
        crumbs.addView(button("‹") { goUp() }, LinearLayout.LayoutParams(dp(48), dp(48)))
        val path = if (archive != null) archive!!.source.name + if (archive!!.prefix.isEmpty()) "" else " / ${archive!!.prefix.trimEnd('/')}" else folder?.let { if (it.isDocument) "已授权 / ${it.name}" else it.location.removePrefix(storage.shared.path).ifEmpty { "内部存储" } } ?: "文件"
        crumbs.addView(text(path, 13f, muted).apply { maxLines = 2; setPadding(dp(8), 0, 0, 0); setOnClickListener { if (archive != null) closeArchive() else folderInfo() } }, LinearLayout.LayoutParams(0, -2, 1f))
        layout.addView(crumbs)
        val tools = row().apply { setPadding(dp(16), 0, dp(16), dp(8)) }
        val search = EditText(this).apply { hint = "搜索当前目录"; textSize = 14f; setSingleLine(); setTextColor(ink); setHintTextColor(muted); background = rounded(surface, 12); setPadding(dp(14), dp(10), dp(14), dp(10)); setText(query) }
        tools.addView(search, LinearLayout.LayoutParams(0, dp(48), 1f))
        val sorting = button("排序") { sortMenu() }; tools.addView(sorting, LinearLayout.LayoutParams(dp(64), dp(48)).apply { marginStart = dp(8) })
        layout.addView(tools)
        val selectionBar = row().apply { setPadding(dp(16), 0, dp(16), dp(8)); visibility = if (selected.isEmpty()) View.GONE else View.VISIBLE }
        layout.addView(selectionBar)
        val panel = FrameLayout(this); layout.addView(panel, LinearLayout.LayoutParams(-1, 0, 1f))
        val list = ListView(this).apply { divider = null; setPadding(dp(12), 0, dp(12), dp(8)); clipToPadding = false; scrollBarStyle = View.SCROLLBARS_OUTSIDE_OVERLAY }
        val empty = text(if (loading) "读取中…" else "这个目录是空的", 14f, muted).apply { gravity = Gravity.CENTER; setPadding(dp(24), dp(24), dp(24), dp(24)) }
        panel.addView(list); panel.addView(empty, FrameLayout.LayoutParams(-1, -1)); list.emptyView = empty
        fun refreshRows() {
            visible = Rules.sorted(files.filter { it.name.contains(query, true) }, sort)
            list.adapter = FileAdapter(visible)
            selectionBar.removeAllViews(); selectionBar.visibility = if (selected.isEmpty()) View.GONE else View.VISIBLE
            if (selected.isNotEmpty()) {
                selectionBar.addView(text("已选 ${selected.size} 项", 13f, muted), LinearLayout.LayoutParams(0, -2, 1f))
                selectionBar.add(button("打包 ZIP") { createZip(files.filter { it.location in selected }) })
                selectionBar.add(button("取消") { selected.clear(); refreshRows() })
            }
            empty.text = if (loading) "读取中…" else if (query.isNotEmpty()) "没有匹配的文件" else "这个目录是空的\n可以选择文件，或授权一个文件夹"
        }
        search.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) { query = s?.toString() ?: ""; refreshRows() }
            override fun afterTextChanged(s: Editable?) {}
        })
        list.setOnItemClickListener { _, _, position, _ ->
            val node = visible[position]
            if (selected.isNotEmpty()) { if (!selected.add(node.location)) selected.remove(node.location); refreshRows() }
            else if (archive != null) {
                if (node.directory) listArchive(archive!!.source, emptyList(), node.location.trimEnd('/') + "/", archive!!.staged)
                else AlertDialog.Builder(this).setTitle(node.name).setMessage("大小：${Rules.size(node.size)}\n\n解压整个压缩包后，可在结果目录打开或分享文件。").setPositiveButton("解压") { _, _ -> extractDialog(archive!!.source, emptyList()) }.setNegativeButton("关闭", null).show()
            } else if (node.directory) { folder?.let { parents.add(it) }; navigate(node, clearParents = false) }
            else listArchive(node, files)
        }
        list.setOnItemLongClickListener { _, _, position, _ ->
            val node = visible[position]
            if (archive == null) fileMenu(node) else AlertDialog.Builder(this).setTitle(node.name).setMessage("路径：${node.location}\n大小：${Rules.size(node.size)}").setPositiveButton("关闭", null).show()
            true
        }
        if (archive != null) {
            val page = archive!!
            files = page.rows
            val actions = row().apply { setPadding(dp(16), dp(8), dp(10), dp(10)) }
            actions.add(button("解压", true) { extractDialog(page.source, emptyList()) }, 1f)
            actions.add(button("检查") { submit(Job("test", listOf(page.source), staged = page.staged), showTasks = true) }, 1f)
            actions.add(button("详情") { archiveInfo(page) }, 1f)
            layout.addView(actions)
            if (page.truncated) layout.addView(text("此目录仅显示前 50,000 项，解压不受此限制。", 12f, muted).apply { setPadding(dp(16), 0, dp(16), dp(8)) })
            refreshRows()
        } else {
            refreshRows(); val token = ++epoch; loading = true
            io.execute {
                val result = runCatching { storage.children(requireNotNull(folder)) }
                runOnUiThread {
                    if (token != epoch || tab != 0 || archive != null || isDestroyed) return@runOnUiThread
                    loading = false
                    result.fold(onSuccess = { files = it; refreshRows() }, onFailure = { files = emptyList(); refreshRows(); empty.text = it.message ?: "无法读取，请重新授权文件夹" })
                }
            }
        }
    }
    private inner class FileAdapter(private val rows: List<Node>) : BaseAdapter() {
        override fun getCount() = rows.size
        override fun getItem(position: Int) = rows[position]
        override fun getItemId(position: Int) = position.toLong()
        override fun getView(position: Int, convertView: View?, parent: android.view.ViewGroup?): View {
            val container = convertView as? LinearLayout ?: row().apply {
                setPadding(dp(8), dp(8), dp(8), dp(8)); minimumHeight = dp(72)
                addView(text("", 22f, green).apply { gravity = Gravity.CENTER; background = rounded(tint, 12) }, LinearLayout.LayoutParams(dp(48), dp(48)))
                addView(column().apply { setPadding(dp(14), 0, dp(8), 0); addView(text("", 16f).apply { maxLines = 2 }); addView(text("", 12f, muted).apply { maxLines = 1 }) }, LinearLayout.LayoutParams(0, -2, 1f))
                addView(text("", 18f, muted), LinearLayout.LayoutParams(dp(24), -2))
            }
            val node = rows[position]; val checked = node.location in selected
            (container.getChildAt(0) as TextView).text = if (checked) "✓" else if (node.directory) "▰" else "▤"
            val labels = container.getChildAt(1) as LinearLayout
            (labels.getChildAt(0) as TextView).text = node.name
            (labels.getChildAt(1) as TextView).text = (if (node.directory) "文件夹" else Rules.size(node.size)) + if (node.modified > 0) " · " + DateFormat.getDateInstance(DateFormat.SHORT).format(Date(node.modified)) else ""
            (container.getChildAt(2) as TextView).text = if (node.directory) "›" else ""
            container.background = rounded(if (checked) tint else bg, 12)
            return container
        }
    }
    private fun navigate(node: Node, clearParents: Boolean = true) { if (clearParents) parents.clear(); folder = node; archive = null; query = ""; files = emptyList(); selected.clear(); epoch++; tab = 0; render() }
    private fun closeArchive() { archive = null; selected.clear(); query = ""; render() }
    private fun goUp() {
        val page = archive
        if (page != null) {
            if (page.prefix.isEmpty()) closeArchive() else listArchive(page.source, emptyList(), page.prefix.trimEnd('/').substringBeforeLast('/', "").let { if (it.isEmpty()) "" else "$it/" }, page.staged)
        } else if (parents.isNotEmpty()) navigate(parents.removeAt(parents.lastIndex), false)
        else if (folder?.isDocument == false && folder!!.file.canonicalPath != storage.shared.canonicalPath && folder!!.file.canonicalPath != storage.results.canonicalPath) {
            folder!!.file.parentFile?.let { parent ->
                if (parent.canonicalPath.startsWith(storage.shared.canonicalPath) || parent.canonicalPath.startsWith(storage.results.canonicalPath)) navigate(storage.fromFile(parent))
            }
        }
    }
    @Deprecated("Deprecated in Java") override fun onBackPressed() {
        if (selected.isNotEmpty()) { selected.clear(); render(); return }
        if (tab != 0) { switchTab(0); return }
        if (archive != null || parents.isNotEmpty()) { goUp(); return }
        super.onBackPressed()
    }
    private fun sortMenu() {
        val labels = arrayOf("名称", "大小（大到小）", "修改时间（新到旧）")
        AlertDialog.Builder(this).setTitle("排序方式").setItems(labels) { _, i -> sort = arrayOf("name", "size", "date")[i]; prefs.edit().putString("sort", sort).apply(); render() }.show()
    }
    private fun fileMenu(node: Node) {
        AlertDialog.Builder(this).setTitle(node.name).setItems(arrayOf("打开压缩包", "解压", "检查完整性", "选择 / 多选打包", "文件信息", "分享文件")) { _, i -> when (i) {
            0 -> listArchive(node, files); 1 -> if (node.directory) notify("请选择压缩包文件") else extractDialog(node, files)
            2 -> if (node.directory) notify("请选择压缩包文件") else submit(Job("test", listOf(node), files), true)
            3 -> { selected.add(node.location); render() }; 4 -> nodeInfo(node); 5 -> if (node.directory) notify("文件夹可以先打包为 ZIP") else share(node)
        } }.show()
    }
    private fun mainMenu() {
        AlertDialog.Builder(this).setTitle("MilkZip").setItems(arrayOf("选择文件 / 分卷", "授权文件夹", "打包所选文件", "设置", "关于 MilkZip", "刷新")) { _, i -> when (i) {
            0 -> pickFiles(); 1 -> pickTree(102); 2 -> if (selected.isEmpty()) { notify("长按文件后选择“多选打包”，也可通过系统选择器选文件"); pickFiles() } else createZip(files.filter { it.location in selected })
            3 -> settings(); 4 -> about(); 5 -> render()
        } }.show()
    }
    private fun storageAccess() {
        AlertDialog.Builder(this).setTitle("浏览手机存储").setMessage("允许文件访问后，可以直接浏览下载目录、读取同目录分卷并保存解压结果。也可以只授权一个文件夹。")
            .setPositiveButton("允许文件访问") { _, _ ->
                if (Build.VERSION.SDK_INT >= 30) {
                    runCatching { startActivityForResult(Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:$packageName")), 105) }
                        .onFailure { startActivityForResult(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION), 105) }
                } else requestPermissions(arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE), 106)
            }.setNeutralButton("授权文件夹") { _, _ -> pickTree(102) }.setNegativeButton("取消", null).show()
    }
    private fun pickFiles() { startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { addCategory(Intent.CATEGORY_OPENABLE); type = "*/*"; putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true); addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION) }, 101) }
    private fun pickTree(code: Int) { startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION or Intent.FLAG_GRANT_PREFIX_URI_PERMISSION), code) }
    private fun persist(uri: Uri, data: Intent) {
        val flags = if (data.flags and Intent.FLAG_GRANT_WRITE_URI_PERMISSION != 0)
            Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION else Intent.FLAG_GRANT_READ_URI_PERMISSION
        runCatching { contentResolver.takePersistableUriPermission(uri, flags) }
    }
    @Deprecated("Deprecated in Java") override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == 105) { if (storage.allFiles) navigate(storage.fromFile(storage.shared)); return }
        if (resultCode != RESULT_OK || data == null) { if (requestCode == 103) pendingExtract = null; return }
        when (requestCode) {
            101 -> {
                val uris = if (data.clipData != null) (0 until data.clipData!!.itemCount).map { data.clipData!!.getItemAt(it).uri } else listOfNotNull(data.data)
                uris.forEach { persist(it, data) }
                io.execute {
                    val result = runCatching { uris.map { storage.fromUri(it) } }
                    runOnUiThread { result.fold({ chooseSources(it) }, ::failure) }
                }
            }
            102 -> data.data?.let { uri ->
                persist(uri, data)
                runCatching { storage.treeRoot(uri) }.onSuccess { prefs.edit().putString("last-tree", uri.toString()).apply(); navigate(it) }.onFailure(::failure)
            }
            103 -> data.data?.let { uri ->
                persist(uri, data); prefs.edit().putString("output-tree", uri.toString()).apply()
                pendingExtract?.let { extractDialog(it, pendingSiblings, pendingSettings, pendingName) } ?: notify("输出位置已保存")
            }
            108 -> data.data?.let { uri ->
                persist(uri, data)
                prefs.getString("export-pending", null)?.let { path ->
                    submit(Job("export", listOf(storage.fromFile(File(path))), exportTree = uri.toString()), true)
                    prefs.edit().remove("export-pending").apply()
                }
            }
            104 -> data.data?.let { uri ->
                io.execute {
                    val result = runCatching {
                        val bytes = contentResolver.openInputStream(uri)?.use { stream ->
                            val output = java.io.ByteArrayOutputStream(); val buffer = ByteArray(8192)
                            while (output.size() <= 256 * 1024) { val n = stream.read(buffer, 0, minOf(buffer.size, 256 * 1024 + 1 - output.size())); if (n < 0) break; output.write(buffer, 0, n) }
                            output.toByteArray() } ?: error("无法读取文件")
                        require(bytes.size <= 256 * 1024) { "密码文件超过 256 KB" }
                        val decoder = Charsets.UTF_8.newDecoder().onMalformedInput(java.nio.charset.CodingErrorAction.REPORT)
                        val lines = decoder.decode(java.nio.ByteBuffer.wrap(bytes)).toString().removePrefix("\uFEFF").split('\n').map { it.removeSuffix("\r") }.filter { it.isNotEmpty() }.distinct()
                        require(lines.all(Rules::validPassword)) { "导入中存在无效密码" }
                        val merged = (passwordDraft.toList() + lines).distinct()
                        require(merged.size <= 64) { "合并后超过 64 条，请先清理密码本" }
                        val proposed = merged to (merged.size - passwordDraft.size)
                        bytes.fill(0); proposed
                    }
                    runOnUiThread { result.fold({ passwordDraft = it.first.toMutableList(); passwordDirty = true; render(); notify("导入 ${it.second} 条，点击保存后生效") }, ::failure) }
                }
            }
        }
    }
    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 106 && storage.allFiles) navigate(storage.fromFile(storage.shared))
    }
    private fun chooseSources(nodes: List<Node>) {
        if (nodes.isEmpty()) return
        externalSources = nodes
        if (nodes.size == 1) listArchive(nodes.first(), nodes)
        else AlertDialog.Builder(this).setTitle("已选择 ${nodes.size} 个文件").setMessage("如果这些文件属于同一组分卷，打开其中一个即可，其他分卷会一同读取。")
            .setPositiveButton("打开压缩包") { _, _ -> listArchive(nodes.first(), nodes) }
            .setNeutralButton("打包 ZIP") { _, _ -> createZip(nodes) }.setNegativeButton("取消", null).show()
    }
    private fun handleIntent(intent: Intent) {
        val uris = when (intent.action) {
            Intent.ACTION_VIEW -> listOfNotNull(intent.data)
            Intent.ACTION_SEND -> listOfNotNull(if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java) else @Suppress("DEPRECATION") intent.getParcelableExtra(Intent.EXTRA_STREAM))
            Intent.ACTION_SEND_MULTIPLE -> if (Build.VERSION.SDK_INT >= 33) intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM, Uri::class.java)?.toList() ?: emptyList() else @Suppress("DEPRECATION") intent.getParcelableArrayListExtra<Uri>(Intent.EXTRA_STREAM)?.toList() ?: emptyList()
            else -> emptyList()
        }
        if (uris.isNotEmpty()) io.execute {
            val result = runCatching { uris.map { storage.fromUri(it) } }
            runOnUiThread { result.fold({ tab = 0; render(); chooseSources(it) }, ::failure) }
        }
    }
    private fun listArchive(node: Node, siblings: List<Node>, prefix: String = "", staged: File? = null) {
        if (node.directory) { navigate(node); return }
        submit(Job("list", listOf(node), siblings, prefix, staged), false)
    }
    private fun submit(job: Job, showTasks: Boolean) {
        try {
            if (!Tasks.submit(job)) { notify("已有任务正在运行，可在任务页取消"); return }
            if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED && !prefs.getBoolean("notification-asked", false)) {
                prefs.edit().putBoolean("notification-asked", true).apply(); requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 107)
            }
            if (showTasks) { tab = 1; render() }
        } catch (error: Exception) { failure(error) }
    }
    private fun extractDialog(source: Node, siblings: List<Node>, initial: ExtractSettings? = null, initialName: String = "") {
        if (Tasks.state != null) { notify("请先等待或取消当前任务"); return }
        val options = initial ?: ExtractSettings(prefs.getBoolean("recursive", true), prefs.getBoolean("ask", false), prefs.getBoolean("keep", true), prefs.getInt("depth", 32), prefs.getLong("limit", 64L shl 30))
        val form = padded(column(), 20)
        val name = EditText(this).apply { hint = "结果文件夹名称"; setSingleLine(); setText(initialName.ifEmpty { source.name.take(180) + "_解压" }); textSize = 15f }
        form.add(text("结果文件夹", 12f, muted)); form.add(name)
        val recursive = CheckBox(this).apply { text = "自动解开内层压缩包"; isChecked = options.recursive }
        val ask = CheckBox(this).apply { text = "每一层由我选择继续解压的文件"; isChecked = options.ask }
        val keep = CheckBox(this).apply { text = "保留解出的中间压缩包"; isChecked = options.keep }
        form.add(recursive); form.add(ask); form.add(keep)
        val depth = Spinner(this).apply { adapter = ArrayAdapter(this@MainActivity, android.R.layout.simple_spinner_dropdown_item, listOf("最多 8 层", "最多 16 层", "最多 32 层")); setSelection(listOf(8, 16, 32).indexOf(options.depth).coerceAtLeast(0)) }
        val limits = Spinner(this).apply { adapter = ArrayAdapter(this@MainActivity, android.R.layout.simple_spinner_dropdown_item, listOf("最多输出 4 GB", "最多输出 16 GB", "最多输出 64 GB")); setSelection(listOf(4L shl 30, 16L shl 30, 64L shl 30).indexOf(options.maxBytes).coerceAtLeast(0)) }
        form.add(depth); form.add(limits)
        recursive.setOnCheckedChangeListener { _, checked -> ask.isEnabled = checked; keep.isEnabled = checked; depth.isEnabled = checked }
        ask.isEnabled = recursive.isChecked; keep.isEnabled = recursive.isChecked; depth.isEnabled = recursive.isChecked
        val locations = mutableListOf("应用结果目录")
        val canSibling = !source.isDocument && source.file.parentFile?.canWrite() == true
        if (canSibling) locations.add("原文件所在目录")
        val tree = prefs.getString("output-tree", null)
        if (tree != null) locations.add("已授权的输出文件夹")
        val location = Spinner(this).apply { adapter = ArrayAdapter(this@MainActivity, android.R.layout.simple_spinner_dropdown_item, locations); setSelection(if (tree != null) locations.lastIndex else if (canSibling) 1 else 0) }
        form.add(text("保存到", 12f, muted)); form.add(location)
        lateinit var dialog: AlertDialog
        fun settings() = ExtractSettings(recursive.isChecked, ask.isChecked, keep.isChecked, listOf(8, 16, 32)[depth.selectedItemPosition], listOf(4L shl 30, 16L shl 30, 64L shl 30)[limits.selectedItemPosition])
        form.add(button("选择其他输出文件夹…") {
            pendingExtract = source; pendingSiblings = siblings; pendingSettings = settings(); pendingName = name.text.toString(); dialog.dismiss(); pickTree(103)
        })
        form.add(text("不会覆盖原文件。重名结果会自动编号；输出上限同时受剩余空间限制。", 12f, muted))
        dialog = AlertDialog.Builder(this).setTitle("解压 ${source.name}").setView(ScrollView(this).apply { addView(form) })
            .setPositiveButton("开始解压", null).setNegativeButton("取消", null).create()
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                try {
                    val outputName = Rules.safeName(name.text.toString()); val s = settings()
                    prefs.edit().putBoolean("recursive", s.recursive).putBoolean("ask", s.ask).putBoolean("keep", s.keep).putInt("depth", s.depth).putLong("limit", s.maxBytes).apply()
                    val choice = locations[location.selectedItemPosition]
                    val siblingFiles = if (siblings.isNotEmpty()) siblings else externalSources
                    val staged = archive?.takeIf { it.source.location == source.location }?.staged
                    submit(Job("extract", listOf(source), siblingFiles, staged = staged, settings = s,
                        parent = if (choice == "原文件所在目录") source.file.parentFile else null, name = outputName,
                        exportTree = if (choice == "已授权的输出文件夹") tree else null), true)
                    pendingExtract = null; dialog.dismiss()
                } catch (error: Exception) { failure(error) }
            }
        }
        dialog.show()
    }
    private fun createZip(nodes: List<Node>) {
        if (nodes.isEmpty()) { notify("请先选择文件"); return }
        val edit = EditText(this).apply { setSingleLine(); setText("打包-${java.text.SimpleDateFormat("MMdd-HHmm", java.util.Locale.ROOT).format(Date())}.zip") }
        AlertDialog.Builder(this).setTitle("创建 ZIP").setMessage("${nodes.size} 项；ZIP 存储模式，不压缩、不加密。结果保存在应用结果目录。")
            .setView(padded(column(), 20).apply { add(edit) }).setPositiveButton("创建", null).setNegativeButton("取消", null).create().apply {
                setOnShowListener { getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                    runCatching { val name = edit.text.toString().let { if (it.endsWith(".zip", true)) it else "$it.zip" }; Rules.safeName(name); submit(Job("zip", nodes, name = name), true); selected.clear(); dismiss() }.onFailure(::failure)
                } }; show()
            }
    }
    private fun renderTasks() {
        val scroll = ScrollView(this); val layout = padded(column()); scroll.addView(layout); body.addView(scroll)
        if (Tasks.state != null) {
            val card = padded(column()).apply { background = rounded(surface) }
            activeText = text("", 15f); activeProgress = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply { max = 1000 }
            card.add(activeText!!); card.add(activeProgress!!); card.add(button("取消任务") { Tasks.cancel() }); layout.add(card)
        }
        if (Tasks.records.isEmpty()) layout.add(text("还没有任务\n选择一个文件，开始解压或检查。", 15f, muted).apply { setPadding(0, dp(32), 0, 0) })
        else {
            val heading = row(); heading.addView(text("最近记录", 13f, muted), LinearLayout.LayoutParams(0, -2, 1f)); heading.add(button("清空") {
                AlertDialog.Builder(this).setTitle("清空任务记录？").setMessage("已解压的文件会保留。").setPositiveButton("清空") { _, _ -> Tasks.clearHistory(); render() }.setNegativeButton("取消", null).show()
            }); layout.add(heading)
            Tasks.records.forEach { record ->
                val card = padded(column()).apply { background = rounded(surface) }
                card.add(text(Rules.resultLabel(record.code), 13f, if (record.code == 0) green else if (dark) Color.parseColor("#E6BD8A") else Color.parseColor("#94622E")))
                card.add(text(record.title, 16f).apply { setTypeface(null, Typeface.BOLD) })
                card.add(text(DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT).format(Date(record.time)), 12f, muted))
                if (record.detail.isNotEmpty()) card.add(text(record.detail.lineSequence().firstOrNull().orEmpty(), 13f, muted).apply { maxLines = 2 })
                val actions = row(); actions.add(button("查看报告") { report(record) }, 1f)
                record.output?.let { output -> actions.add(button("查看结果") { navigate(storage.fromFile(File(output))) }, 1f) }
                card.add(actions); layout.add(card)
            }
        }
    }
    private fun report(record: TaskRecord) {
        val form = padded(column(), 20)
        form.add(text(record.title, 16f)); form.add(text(record.detail.ifEmpty { Rules.resultLabel(record.code) }, 13f).apply { setTextIsSelectable(true) })
        record.output?.let { output ->
            form.add(button("导出结果到其他文件夹") { pendingExtract = null; prefs.edit().putString("export-pending", output).apply(); pickTree(108) })
        }
        AlertDialog.Builder(this).setTitle(Rules.resultLabel(record.code)).setView(ScrollView(this).apply { addView(form) }).setPositiveButton("关闭", null).setNeutralButton("复制报告") { _, _ ->
            (getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager).setPrimaryClip(ClipData.newPlainText("MilkZip 任务报告", record.title + "\n" + record.detail)); notify("报告已复制")
        }.show()
    }
    private fun updateTask() {
        if (Tasks.pageVersion != pageVersion) {
            pageVersion = Tasks.pageVersion
            Tasks.page?.let { archive = it; files = it.rows; query = ""; tab = 0; selected.clear(); render() }
        }
        val latest = Tasks.records.firstOrNull()
        if (latest?.id != latestRecord) {
            latestRecord = latest?.id
            if (tab == 1) render()
            if (tab == 2 && !passwordDirty) loadPasswords()
            if (latest != null && resumed) notify(Rules.resultLabel(latest.code) + " · " + latest.title)
        }
        updateTaskViews(); showPrompt()
    }
    private fun updateTaskViews() {
        val s = Tasks.state
        mini.visibility = if (s == null) View.GONE else View.VISIBLE
        if (s != null) {
            val fraction = if (s.total > 0) " · ${Rules.size(s.done)} / ${Rules.size(s.total)}" else ""
            mini.text = s.phase + fraction + "  ›"
            activeText?.text = s.title + "\n" + s.phase + fraction
            activeProgress?.isIndeterminate = s.total <= 0
            activeProgress?.progress = if (s.total > 0) (s.done.toDouble() / s.total * 1000).toInt().coerceIn(0, 1000) else 0
        }
    }
    private fun showPrompt() {
        val p = prompt
        if (!resumed || p == null) { if (p == null) { promptDialog?.dismiss(); promptDialog = null; promptId = null }; return }
        if (promptId == p.id && promptDialog?.isShowing == true) return
        promptDialog?.dismiss(); promptId = p.id
        if (p.branches != null) {
            var choice = 0
            promptDialog = AlertDialog.Builder(this).setTitle("继续解开哪些文件？")
                .setSingleChoiceItems(p.branches.toTypedArray(), 0) { _, i -> choice = i }
                .setPositiveButton("解开所选") { _, _ -> Tasks.reply(p.id, PromptReply(choice = choice)) }
                .setNeutralButton("全部解开") { _, _ -> Tasks.reply(p.id, PromptReply(choice = -3)) }
                .setNegativeButton("到这一层结束") { _, _ -> Tasks.reply(p.id, PromptReply(choice = -1)) }
                .setOnCancelListener { Tasks.cancel() }.create()
        } else {
            window.addFlags(WindowManager.LayoutParams.FLAG_SECURE)
            val form = padded(column(), 20)
            form.add(text(File(p.path).name, 14f, muted))
            if (p.rejected) form.add(text("密码不正确，请重新输入", 14f, if (dark) Color.parseColor("#FFB4A9") else Color.parseColor("#A73626")))
            val password = passwordInput()
            val save = CheckBox(this).apply { text = "验证成功后保存到密码本" }
            val show = CheckBox(this).apply { text = "显示密码"; setOnCheckedChangeListener { _, checked -> password.transformationMethod = if (checked) null else PasswordTransformationMethod.getInstance() } }
            form.add(password); form.add(show); form.add(save)
            promptDialog = AlertDialog.Builder(this).setTitle("需要解压密码").setView(form).setPositiveButton("继续", null).setNegativeButton("取消任务") { _, _ -> Tasks.cancel() }
                .setOnCancelListener { Tasks.cancel() }.create()
            promptDialog!!.setOnShowListener { promptDialog!!.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val value = password.text.toString()
                if (!Rules.validPassword(value)) { password.error = "请输入 1–1024 字节的密码"; return@setOnClickListener }
                val chars = value.toCharArray(); password.text.clear(); Tasks.reply(p.id, PromptReply(chars, save.isChecked)); promptDialog?.dismiss()
            } }
            promptDialog!!.setOnDismissListener { password.text.clear(); if (tab != 2) window.clearFlags(WindowManager.LayoutParams.FLAG_SECURE) }
        }
        promptDialog!!.setCanceledOnTouchOutside(false); promptDialog!!.show()
    }
    private fun passwordInput() = EditText(this).apply { inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS; isSaveEnabled = false; setSingleLine(); hint = "密码（保留首尾空格）"; importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS }
    private fun loadPasswords() {
        runCatching { PasswordBook(this).read() }.fold({ passwordDraft = it.toMutableList(); passwordDirty = false; render() }, {
            passwordDraft.clear(); render(); AlertDialog.Builder(this).setTitle("密码本无法读取").setMessage("${it.message}\n现有加密文件没有被覆盖。若密钥已经失效，可重置密码本后重新添加。")
                .setPositiveButton("关闭", null).setNegativeButton("重置密码本") { _, _ -> AlertDialog.Builder(this).setTitle("删除原密码本并重置？").setMessage("此操作会清空本机已保存的密码。")
                    .setPositiveButton("重置") { _, _ -> runCatching { PasswordBook(this).write(emptyList()) }.onSuccess { loadPasswords() }.onFailure(::failure) }.setNegativeButton("取消", null).show() }.show()
        })
    }
    private fun renderPasswords() {
        val layout = padded(column()); body.addView(layout)
        layout.add(text("Android 系统密钥加密 · 最多 64 条\n只会自动保存已验证成功的解压密码。", 13f, muted))
        val actions = row(); actions.add(button("添加") { editPassword() }, 1f); actions.add(button("导入") {
            startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { addCategory(Intent.CATEGORY_OPENABLE); type = "*/*" }, 104)
        }, 1f); actions.add(button(if (passwordDirty) "保存 *" else "保存", true) { if (savePasswords()) render() }, 1f); layout.add(actions)
        val reveal = CheckBox(this).apply { text = "显示密码"; isChecked = revealPasswords; setOnCheckedChangeListener { _, value -> revealPasswords = value; render() } }; layout.add(reveal)
        val list = ListView(this).apply { divider = null }
        list.adapter = object : BaseAdapter() {
            override fun getCount() = passwordDraft.size
            override fun getItem(position: Int) = passwordDraft[position]
            override fun getItemId(position: Int) = position.toLong()
            override fun getView(position: Int, convertView: View?, parent: android.view.ViewGroup?): View = row().apply {
                setPadding(dp(12), dp(12), dp(12), dp(12)); minimumHeight = dp(60); background = rounded(surface, 12)
                addView(text("${position + 1}", 13f, muted), LinearLayout.LayoutParams(dp(36), -2))
                addView(text(if (revealPasswords) passwordDraft[position] else "••••••••", 16f).apply { maxLines = 2 }, LinearLayout.LayoutParams(0, -2, 1f))
                addView(text("⋮", 22f, muted), LinearLayout.LayoutParams(dp(32), -2))
            }
        }
        list.setOnItemClickListener { _, _, position, _ ->
            AlertDialog.Builder(this).setTitle("密码 ${position + 1}").setItems(arrayOf("修改", "上移", "下移", "删除")) { _, i ->
                when (i) { 0 -> { editPassword(position); return@setItems }; 1 -> if (position > 0) java.util.Collections.swap(passwordDraft, position, position - 1)
                    2 -> if (position < passwordDraft.lastIndex) java.util.Collections.swap(passwordDraft, position, position + 1); 3 -> passwordDraft.removeAt(position) }
                passwordDirty = true; render()
            }.show()
        }
        layout.addView(list, LinearLayout.LayoutParams(-1, 0, 1f))
        if (passwordDraft.isEmpty()) layout.add(text("还没有保存的密码\n解压时可选择“验证成功后保存”。", 14f, muted))
    }
    private fun savePasswords(): Boolean = try { PasswordBook(this).write(passwordDraft); passwordDirty = false; notify("密码本已保存"); true } catch (error: Exception) { failure(error); false }
    private fun editPassword(index: Int? = null) {
        val input = passwordInput().apply { index?.let { setText(passwordDraft[it]) } }
        val form = padded(column(), 20).apply { add(input); add(CheckBox(this@MainActivity).apply { text = "显示密码"; setOnCheckedChangeListener { _, checked -> input.transformationMethod = if (checked) null else PasswordTransformationMethod.getInstance() } }) }
        AlertDialog.Builder(this).setTitle(if (index == null) "添加密码" else "修改密码").setView(form).setPositiveButton("确定", null).setNegativeButton("取消", null).create().apply {
            setOnShowListener { getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val value = input.text.toString()
                if (!Rules.validPassword(value)) { input.error = "请输入 1–1024 字节的密码"; return@setOnClickListener }
                if (passwordDraft.withIndex().any { it.index != index && it.value == value }) { input.error = "密码已存在"; return@setOnClickListener }
                if (index == null && passwordDraft.size >= 64) { input.error = "最多 64 条密码"; return@setOnClickListener }
                if (index == null) passwordDraft.add(value) else passwordDraft[index] = value
                passwordDirty = true; input.text.clear(); dismiss(); render()
            } }; setOnDismissListener { input.text.clear() }; show()
        }
    }
    private fun archiveInfo(page: ArchivePage) {
        AlertDialog.Builder(this).setTitle(page.source.name).setMessage("文件数量：${page.count}\n原文件：${Rules.size(page.source.size)}\n条目压缩大小：${Rules.size(page.packed)}\n解压后大小：${Rules.size(page.size)}\n\n按内容识别格式；支持改后缀、分卷和多层嵌套。自动递归会保留 DOCX、EPUB、APK 等应用文件。")
            .setPositiveButton("关闭", null).show()
    }
    private fun nodeInfo(node: Node) { AlertDialog.Builder(this).setTitle(node.name).setMessage("位置：${node.location}\n大小：${Rules.size(node.size)}\n修改：${DateFormat.getDateTimeInstance().format(Date(node.modified))}").setPositiveButton("关闭", null).show() }
    private fun folderInfo() { folder?.let { nodeInfo(it) } }
    private fun share(node: Node) {
        io.execute {
            val result = runCatching { storage.shareCopy(node) }
            runOnUiThread { result.fold({ file ->
                val uri = FileProvider.getUriForFile(this, "$packageName.files", file)
                runCatching { startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).setType("application/octet-stream").putExtra(Intent.EXTRA_STREAM, uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION), "分享文件")) }.onFailure(::failure)
            }, ::failure) }
        }
    }
    private fun settings() {
        AlertDialog.Builder(this).setTitle("设置").setItems(arrayOf("文件访问权限", "授权输出文件夹", "使用应用结果目录", "打开上次授权的文件夹")) { _, i -> when (i) {
            0 -> storageAccess(); 1 -> { pendingExtract = null; pickTree(103) }; 2 -> { prefs.edit().remove("output-tree").apply(); notify("默认输出位置已重置") }
            3 -> prefs.getString("last-tree", null)?.let { runCatching { storage.treeRoot(Uri.parse(it)) }.onSuccess { node -> navigate(node) }.onFailure(::failure) } ?: notify("尚未授权文件夹")
        } }.show()
    }
    private fun about() {
        AlertDialog.Builder(this).setTitle("MilkZip 0.6.0 Android 预览版").setMessage("轻松解压，一层到底\n\n与 Windows 版共用 7-Zip + LZ4 核心。支持 ZIP、7z、RAR、TAR、GZip、BZip2、XZ、Zstandard、LZ4、LZMA、传统 .Z 等格式，包含分卷、密码、多层递归及 ZIP 打包。\n\n完全离线，无广告，无网络权限。Android 8.0 及以上，ARM64 / x86_64。\n\nMilkZip：MIT\n7-Zip 26.04：GNU LGPL + unRAR 限制\nLZ4 1.10.0：BSD 2-Clause\n\n完整许可证见 APK assets/licenses 和 GitHub 仓库。")
            .setPositiveButton("关闭", null).setNeutralButton("开源仓库") { _, _ -> startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("https://github.com/qgmzxsn/milkzip"))) }.show()
    }
}
