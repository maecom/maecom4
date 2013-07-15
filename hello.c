  1 /*
  2  * linux/drivers/char/misc.c
  3  *
  4  * Generic misc open routine by Johan Myreen
  5  *
  6  * Based on code from Linus
  7  *
  8  * Teemu Rantanen's Microsoft Busmouse support and Derrick Cole's
  9  *   changes incorporated into 0.97pl4
 10  *   by Peter Cervasio (pete%q106fm.uucp@wupost.wustl.edu) (08SEP92)
 11  *   See busmouse.c for particulars.
 12  *
 13  * Made things a lot mode modular - easy to compile in just one or two
 14  * of the misc drivers, as they are now completely independent. Linus.
 15  *
 16  * Support for loadable modules. 8-Sep-95 Philip Blundell <pjb27@cam.ac.uk>
 17  *
 18  * Fixed a failing symbol register to free the device registration
 19  *              Alan Cox <alan@lxorguk.ukuu.org.uk> 21-Jan-96
 20  *
 21  * Dynamic minors and /proc/mice by Alessandro Rubini. 26-Mar-96
 22  *
 23  * Renamed to misc and miscdevice to be more accurate. Alan Cox 26-Mar-96
 24  *
 25  * Handling of mouse minor numbers for kerneld:
 26  *  Idea by Jacques Gelinas <jack@solucorp.qc.ca>,
 27  *  adapted by Bjorn Ekwall <bj0rn@blox.se>
 28  *  corrected by Alan Cox <alan@lxorguk.ukuu.org.uk>
 29  *
 30  * Changes for kmod (from kerneld):
 31  *      Cyrus Durgin <cider@speakeasy.org>
 32  *
 33  * Added devfs support. Richard Gooch <rgooch@atnf.csiro.au>  10-Jan-1998
 34  */
 35 
 36 #include <linux/module.h>
 37 
 38 #include <linux/fs.h>
 39 #include <linux/errno.h>
 40 #include <linux/miscdevice.h>
 41 #include <linux/kernel.h>
 42 #include <linux/major.h>
 43 #include <linux/slab.h>
 44 #include <linux/mutex.h>
 45 #include <linux/proc_fs.h>
 46 #include <linux/seq_file.h>
 47 #include <linux/stat.h>
 48 #include <linux/init.h>
 49 #include <linux/device.h>
 50 #include <linux/tty.h>
 51 #include <linux/kmod.h>
 52 #include <linux/smp_lock.h>
 53 
 54 /*
 55  * Head entry for the doubly linked miscdevice list
 56  */
 57 static LIST_HEAD(misc_list);
 58 static DEFINE_MUTEX(misc_mtx);
 59 
 60 /*
 61  * Assigned numbers, used for dynamic minors
 62  */
 63 #define DYNAMIC_MINORS 64 /* like dynamic majors */
 64 static unsigned char misc_minors[DYNAMIC_MINORS / 8];
 65 
 66 extern int pmu_device_init(void);
 67 
 68 #ifdef CONFIG_PROC_FS
 69 static void *misc_seq_start(struct seq_file *seq, loff_t *pos)
 70 {
 71         mutex_lock(&misc_mtx);
 72         return seq_list_start(&misc_list, *pos);
 73 }
 74 
 75 static void *misc_seq_next(struct seq_file *seq, void *v, loff_t *pos)
 76 {
 77         return seq_list_next(v, &misc_list, pos);
 78 }
 79 
 80 static void misc_seq_stop(struct seq_file *seq, void *v)
 81 {
 82         mutex_unlock(&misc_mtx);
 83 }
 84 
 85 static int misc_seq_show(struct seq_file *seq, void *v)
 86 {
 87         const struct miscdevice *p = list_entry(v, struct miscdevice, list);
 88 
 89         seq_printf(seq, "%3i %s\n", p->minor, p->name ? p->name : "");
 90         return 0;
 91 }
 92 
 93 
 94 static struct seq_operations misc_seq_ops = {
 95         .start = misc_seq_start,
 96         .next  = misc_seq_next,
 97         .stop  = misc_seq_stop,
 98         .show  = misc_seq_show,
 99 };
100 
101 static int misc_seq_open(struct inode *inode, struct file *file)
102 {
103         return seq_open(file, &misc_seq_ops);
104 }
105 
106 static const struct file_operations misc_proc_fops = {
107         .owner   = THIS_MODULE,
108         .open    = misc_seq_open,
109         .read    = seq_read,
110         .llseek  = seq_lseek,
111         .release = seq_release,
112 };
113 #endif
114 
115 static int misc_open(struct inode * inode, struct file * file)
116 {
117         int minor = iminor(inode);
118         struct miscdevice *c;
119         int err = -ENODEV;
120         const struct file_operations *old_fops, *new_fops = NULL;
121         
122         lock_kernel();
123         mutex_lock(&misc_mtx);
124         
125         list_for_each_entry(c, &misc_list, list) {
126                 if (c->minor == minor) {
127                         new_fops = fops_get(c->fops);           
128                         break;
129                 }
130         }
131                 
132         if (!new_fops) {
133                 mutex_unlock(&misc_mtx);
134                 request_module("char-major-%d-%d", MISC_MAJOR, minor);
135                 mutex_lock(&misc_mtx);
136 
137                 list_for_each_entry(c, &misc_list, list) {
138                         if (c->minor == minor) {
139                                 new_fops = fops_get(c->fops);
140                                 break;
141                         }
142                 }
143                 if (!new_fops)
144                         goto fail;
145         }
146 
147         err = 0;
148         old_fops = file->f_op;
149         file->f_op = new_fops;
150         if (file->f_op->open) {
151                 err=file->f_op->open(inode,file);
152                 if (err) {
153                         fops_put(file->f_op);
154                         file->f_op = fops_get(old_fops);
155                 }
156         }
157         fops_put(old_fops);
158 fail:
159         mutex_unlock(&misc_mtx);
160         unlock_kernel();
161         return err;
162 }
163 
164 static struct class *misc_class;
165 
166 static const struct file_operations misc_fops = {
167         .owner          = THIS_MODULE,
168         .open           = misc_open,
169 };
170 
171 /**
172  *      misc_register   -       register a miscellaneous device
173  *      @misc: device structure
174  *      
175  *      Register a miscellaneous device with the kernel. If the minor
176  *      number is set to %MISC_DYNAMIC_MINOR a minor number is assigned
177  *      and placed in the minor field of the structure. For other cases
178  *      the minor number requested is used.
179  *
180  *      The structure passed is linked into the kernel and may not be
181  *      destroyed until it has been unregistered.
182  *
183  *      A zero is returned on success and a negative errno code for
184  *      failure.
185  */
186  
187 int misc_register(struct miscdevice * misc)
188 {
189         struct miscdevice *c;
190         dev_t dev;
191         int err = 0;
192 
193         INIT_LIST_HEAD(&misc->list);
194 
195         mutex_lock(&misc_mtx);
196         list_for_each_entry(c, &misc_list, list) {
197                 if (c->minor == misc->minor) {
198                         mutex_unlock(&misc_mtx);
199                         return -EBUSY;
200                 }
201         }
202 
203         if (misc->minor == MISC_DYNAMIC_MINOR) {
204                 int i = DYNAMIC_MINORS;
205                 while (--i >= 0)
206                         if ( (misc_minors[i>>3] & (1 << (i&7))) == 0)
207                                 break;
208                 if (i<0) {
209                         mutex_unlock(&misc_mtx);
210                         return -EBUSY;
211                 }
212                 misc->minor = i;
213         }
214 
215         if (misc->minor < DYNAMIC_MINORS)
216                 misc_minors[misc->minor >> 3] |= 1 << (misc->minor & 7);
217         dev = MKDEV(MISC_MAJOR, misc->minor);
218 
219         misc->this_device = device_create(misc_class, misc->parent, dev,
220                                           misc, "%s", misc->name);
221         if (IS_ERR(misc->this_device)) {
222                 err = PTR_ERR(misc->this_device);
223                 goto out;
224         }
225 
226         /*
227          * Add it to the front, so that later devices can "override"
228          * earlier defaults
229          */
230         list_add(&misc->list, &misc_list);
231  out:
232         mutex_unlock(&misc_mtx);
233         return err;
234 }
235 
236 /**
237  *      misc_deregister - unregister a miscellaneous device
238  *      @misc: device to unregister
239  *
240  *      Unregister a miscellaneous device that was previously
241  *      successfully registered with misc_register(). Success
242  *      is indicated by a zero return, a negative errno code
243  *      indicates an error.
244  */
245 
246 int misc_deregister(struct miscdevice *misc)
247 {
248         int i = misc->minor;
249 
250         if (list_empty(&misc->list))
251                 return -EINVAL;
252 
253         mutex_lock(&misc_mtx);
254         list_del(&misc->list);
255         device_destroy(misc_class, MKDEV(MISC_MAJOR, misc->minor));
256         if (i < DYNAMIC_MINORS && i>0) {
257                 misc_minors[i>>3] &= ~(1 << (misc->minor & 7));
258         }
259         mutex_unlock(&misc_mtx);
260         return 0;
261 }
262 
263 EXPORT_SYMBOL(misc_register);
264 EXPORT_SYMBOL(misc_deregister);
265 
266 static char *misc_nodename(struct device *dev)
267 {
268         struct miscdevice *c = dev_get_drvdata(dev);
269 
270         if (c->devnode)
271                 return kstrdup(c->devnode, GFP_KERNEL);
272         return NULL;
273 }
274 
275 static int __init misc_init(void)
276 {
277         int err;
278 
279 #ifdef CONFIG_PROC_FS
280         proc_create("misc", 0, NULL, &misc_proc_fops);
281 #endif
282         misc_class = class_create(THIS_MODULE, "misc");
283         err = PTR_ERR(misc_class);
284         if (IS_ERR(misc_class))
285                 goto fail_remove;
286 
287         err = -EIO;
288         if (register_chrdev(MISC_MAJOR,"misc",&misc_fops))
289                 goto fail_printk;
290         misc_class->nodename = misc_nodename;
291         return 0;
292 
293 fail_printk:
294         printk("unable to get major %d for misc devices\n", MISC_MAJOR);
295         class_destroy(misc_class);
296 fail_remove:
297         remove_proc_entry("misc", NULL);
298         return err;
299 }
300 subsys_initcall(misc_init);
301 
