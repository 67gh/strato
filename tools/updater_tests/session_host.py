from pathlib import Path
import subprocess,tempfile,os
import argparse
parser=argparse.ArgumentParser(description="Host API doubles: tests production session logic, not Android installation")
parser.add_argument('--dependencies',type=Path,required=True)
args=parser.parse_args()
root=Path(__file__).resolve().parents[2];deps=args.dependencies.resolve()
sources={
'context.kt':'''package android.content
import android.content.pm.PackageManager
open class Context {
 val packageName="org.stratoemu.strato"
 val packageManager=PackageManager()
 val preferences=SharedPreferences()
 fun getSharedPreferences(name:String, mode:Int)=preferences
 companion object { const val MODE_PRIVATE=0 }
}
class SharedPreferences {
 val data=mutableMapOf<String,Any>()
 fun contains(k:String)=data.containsKey(k)
 fun getInt(k:String,d:Int)=data[k] as? Int ?:d
 fun getString(k:String,d:String?)=data[k] as? String ?:d
 fun edit()=Editor(this)
 class Editor(val p:SharedPreferences) {
  fun putInt(k:String,v:Int)=apply {p.data[k]=v}
  fun putString(k:String,v:String)=apply {p.data[k]=v}
  fun clear()=apply {p.data.clear()}
  fun apply() {}
 }
}
open class BroadcastReceiver { open fun onReceive(c:Context,i:Intent) {} }
class Intent() {
 constructor(c:Context,t:Class<*>):this()
 var action:String?=null
 val extras=mutableMapOf<String,Any>()
 fun setAction(s:String)=apply {action=s}
 fun putExtra(k:String,v:Any)=apply {extras[k]=v}
 fun getIntExtra(k:String,d:Int)=extras[k] as? Int ?:d
 fun getStringExtra(k:String)=extras[k] as? String
 @Suppress("UNCHECKED_CAST") fun <T> getParcelableExtra(k:String)=extras[k] as? T
 companion object { const val EXTRA_INTENT="intent" }
}
class IntentSender(val intent:Intent)
''',
'pending.kt':'''package android.app
import android.content.*
class PendingIntent(val intentSender:IntentSender) {
 companion object {
 const val FLAG_UPDATE_CURRENT=1; const val FLAG_MUTABLE=2
 var lastFlags=0
 fun getBroadcast(c:Context,id:Int,i:Intent,f:Int):PendingIntent {lastFlags=f;return PendingIntent(IntentSender(i))}
 }
}''',
'build.kt':'''package android.os
object Build { object VERSION {var SDK_INT=34} }''',
'log.kt':'''package android.util
object Log {fun i(t:String,m:String)=0}''',
'package.kt':'''package android.content.pm
import android.content.IntentSender
import java.io.ByteArrayOutputStream
class PackageManager {val packageInstaller=PackageInstaller()}
class PackageInstaller {
 var next=1
 val sessions=mutableMapOf<Int,Session>()
 val abandoned=mutableListOf<Int>()
 var duringCommit:((Int,IntentSender)->Unit)?=null
 var lastParams:SessionParams?=null
 fun createSession(p:SessionParams):Int {lastParams=p;val id=next++;sessions[id]=Session(id,this);return id}
 fun openSession(id:Int)=sessions.getValue(id)
 fun abandonSession(id:Int) {abandoned+=id}
 class SessionParams(val mode:Int) {
  var app="";var length=0L;var userAction=0
  fun setAppPackageName(s:String) {app=s}
  fun setSize(l:Long) {length=l}
  fun setRequireUserAction(v:Int) {userAction=v}
  companion object {const val MODE_FULL_INSTALL=1;const val USER_ACTION_REQUIRED=1}
 }
 class Session(val id:Int,val owner:PackageInstaller):AutoCloseable {
  var closed=false;var synced=false;var committed=false;var writeClosed=false
  val bytes=object:ByteArrayOutputStream(){override fun close(){writeClosed=true;super.close()}}
  fun openWrite(n:String,o:Long,s:Long)=bytes
  fun fsync(o:ByteArrayOutputStream){synced=true}
  fun commit(i:IntentSender){check(writeClosed&&synced);committed=true;owner.duringCommit?.invoke(id,i)}
  override fun close(){closed=true}
 }
 companion object {
 const val STATUS_FAILURE=1; const val STATUS_PENDING_USER_ACTION=-1
 const val EXTRA_SESSION_ID="id";const val EXTRA_STATUS="status";const val EXTRA_STATUS_MESSAGE="message"
 }
}''',
'Test.kt':'''package org.stratoemu.strato.updater
import android.content.*
import android.content.pm.PackageInstaller
import android.app.PendingIntent
import java.nio.file.Files
import kotlinx.coroutines.flow.MutableStateFlow
import java.util.concurrent.CancellationException
fun main() {
 val c=Context();val apk=Files.createTempFile("session", ".apk").toFile()
 try {
  val data=ByteArray(150000){it.toByte()};apk.writeBytes(data)
  val pi=c.packageManager.packageInstaller
  pi.duringCommit={id,sender ->
   UpdateSession.receive(c,sender.intent.putExtra("id",id).putExtra("status",5).putExtra("message","INSTALL_FAILED_UPDATE_INCOMPATIBLE"))
  }
  UpdateSession.install(c,apk,"url"){}
  val first=UpdateSession.events.value!!
  check(first.status==5 && first.detail=="INSTALL_FAILED_UPDATE_INCOMPATIBLE")
  check(pi.sessions.getValue(first.id).bytes.toByteArray().contentEquals(data))
  check(pi.sessions.getValue(first.id).closed)
  check(pi.lastParams!!.app==c.packageName && pi.lastParams!!.length==apk.length())
  check(pi.lastParams!!.userAction==1 && PendingIntent.lastFlags and PendingIntent.FLAG_MUTABLE!=0)
  println("PASS real session code: exact bytes, close/fsync before commit, fast callback preserved, mutable sender")
  val confirm=Intent();val callback=Intent().setAction("org.stratoemu.strato.UPDATE_INSTALL_RESULT")
    .putExtra("id",first.id).putExtra("status",-1).putExtra(Intent.EXTRA_INTENT,confirm)
  UpdateSession.receive(c,callback)
  check(UpdateSession.takeConfirmation()===confirm);check(UpdateSession.takeConfirmation()==null)
  println("PASS confirmation consumed once (rotation does not reopen)")
  UpdateSession.abandon(c)
  UpdateSession.receive(c,callback)
  check(UpdateSession.events.value==null && first.id in pi.abandoned)
  println("PASS obsolete callback ignored after reset/abandon")
  pi.duringCommit=null
  var checks=0
  try {UpdateSession.install(c,apk,"url") {if (++checks==3) throw CancellationException()};error("cancel")}
  catch (_:CancellationException) {}
  val cancelled=UpdateSession.events.value!!
  check(cancelled.id in pi.abandoned && !pi.sessions.getValue(cancelled.id).committed)
  println("PASS cancellation abandons session before commit")
  // Simulate process-local Flow loss, keeping preference bytes.
  c.preferences.data["status"]=UpdateSession.STAGING
  val field=UpdateSession::class.java.getDeclaredField("mutable");field.isAccessible=true
  @Suppress("UNCHECKED_CAST") val flow=field.get(null) as MutableStateFlow<InstallEvent?>
  flow.value=null
  val restored=UpdateSession.restore(c)!!
  check(restored.status==1 && restored.detail.contains("interrupted"))
  UpdateSession.abandon(c)
  check(restored.id in pi.abandoned)
  println("PASS interrupted staging restores as failure, not endless preparation")
 } finally {apk.delete()}
 println("Host API doubles only: no Android installer or phone was executed")
}'''
}
cp=':'.join(str(deps/n) for n in ['kotlin-compiler-embeddable-1.9.22.jar','kotlin-stdlib-1.9.22.jar','kotlin-script-runtime-1.9.22.jar','kotlin-reflect-1.6.10.jar','trove4j-1.0.20200330.jar','annotations-13.0.jar','kotlinx-coroutines-core-jvm-1.6.4.jar'])
with tempfile.TemporaryDirectory() as folder:
 d=Path(folder)
 for name,s in sources.items():(d/name).write_text(s)
 production=root/'app/src/main/java/org/stratoemu/strato/updater'
 cmd=['java','-cp',cp,'org.jetbrains.kotlin.cli.jvm.K2JVMCompiler','-nowarn','-no-stdlib','-no-reflect','-jvm-target','17','-classpath',cp,'-d',str(d/'classes')]+[str(d/n) for n in sources]+[str(production/n) for n in ['UpdateSession.kt','InstallStatusReceiver.kt']]
 subprocess.run(cmd,check=True,timeout=60)
 subprocess.run(['java','-cp',str(d/'classes')+':'+cp,'org.stratoemu.strato.updater.TestKt'],check=True,timeout=30)
